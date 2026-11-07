// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Game state check of the special abilities (XR fork W6), headless, with the converted
 * modpack but without renderer:
 *
 *   openage-special-game-check --root <dir> [--modpack hd_base] [--seed N] [--coast-seed N] [--size N]
 *                              [--ai-limit S] [--only map|monks|relics|siege|ships|save|gaps|ai]
 *
 * Random maps (64 tiles), simulated time advances in steps of 50 ms through the event loop
 * with the production of the simulation (like GameSimulation::run).
 *
 *  S1. Map: relics (gaia, count by map size, on land, not attackable); without the relic
 *      option the map is the one before W6 (same objects, relics appended last).
 *  S2. Monks: the monastery trains monks (Castle Age), faith 100 at the start (data), the
 *      convert/heal values of the data (range 9, 1 s, chance 25, 4 s / 10 s, heal range 4).
 *  S3. Conversion: a monk in range converts an enemy militia between T_min and T_max (+ the
 *      tick), the owner changes (colour, no target, no order), faith 0 afterwards and it
 *      recharges with 1.6/s; a monk without full faith may not start (L45); monks need
 *      Atonement, siege and buildings Redemption; a command "Konvertieren" through the
 *      send-command event works like the order.
 *  S4. Heresy: the target owner researched Heresy – the converted unit dies instead.
 *  S5. Healing: an ordered monk heals a wounded own militia to full health (rate of the
 *      spec with the data values); ram (siege) and buildings are not healable; an idle monk
 *      heals a wounded unit in its search range on its own (stance preference of the data).
 *  S6. Relics: a monk picks up a relic (it is hidden, carried), brings it into the own
 *      monastery (relic gold 0.5/s per relic, 02 §2.10); a relic falls when its carrier dies.
 *  S7. Siege: a mangonel shot damages units around the impact, also own ones (05 §5.5);
 *      a ram attacks buildings only (no units, also not on its own); a trebuchet unpacks
 *      (transform unit of the data) and does not move until it is packed again.
 *  S8. Ships (coast map): a transport ship takes land units (places of the data), they are
 *      hidden, it unloads them at the shore; a sunken transport loses its cargo; the dock
 *      trains galleys.
 *  S9. Save game version 6: faith, a carried relic, relics in a monastery, an unpacked
 *      trebuchet, cargo; save -> load gives them back; a version 5 file loads without relics.
 *  S10. Computer opponent (Castle Age granted): builds a monastery and a siege workshop,
 *      trains monks and siege.
 *
 * Prints "special game check ok" and exits with 0 if everything passes.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "assets/mod_manager.h"
#include "event/event_loop.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/events.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/internal/activity.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/move.h"
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
#include "gamestate/map_generator.h"
#include "gamestate/player.h"
#include "gamestate/production.h"
#include "gamestate/save_format.h"
#include "gamestate/save_game.h"
#include "gamestate/special.h"
#include "gamestate/special_math.h"
#include "gamestate/tech.h"
#include "gamestate/terrain_factory.h"
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
	uint32_t coast_seed = 3;
	size_t size = 64;
	double ai_limit = 900.0;
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

	World(const Options &opt, const MapSettings &base, std::optional<uint32_t> seed = std::nullopt) {
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
		this->settings.seed = seed.value_or(opt.seed);
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

	std::shared_ptr<special::SpecialState> special() const {
		return this->state->get_special();
	}

	std::shared_ptr<combat::CombatState> combat() const {
		return this->state->get_combat();
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

	double distance(const std::shared_ptr<GameEntity> &a, const std::shared_ptr<GameEntity> &b) const {
		auto p = this->position(a);
		auto q = this->position(b);
		return std::hypot(p.ne.to_double() - q.ne.to_double(), p.se.to_double() - q.se.to_double());
	}

	bool alive(const std::shared_ptr<GameEntity> &e) const {
		return e != nullptr and this->state->get_game_entities().contains(e->get_id())
		       and not this->combat()->is_dead(e->get_id());
	}

	std::vector<std::shared_ptr<GameEntity>> named(const std::string &name, bool only_alive = true) const {
		std::vector<std::shared_ptr<GameEntity>> out;
		for (const auto &[id, e] : this->state->get_game_entities()) {
			if (prod::entity_name(e) == name and (not only_alive or not this->combat()->is_dead(id))) {
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

	std::shared_ptr<GameEntity> spawn_at(player_id_t player, const std::string &fqon, double ne, double se) {
		auto entity = this->factory->add_game_entity(this->loop, this->state, player, fqon);
		auto pos = std::dynamic_pointer_cast<component::Position>(entity->get_component(component::component_t::POSITION));
		coord::phys3 at{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}};
		pos->set_position(this->now, this->state->get_map()->on_terrain(at));
		pos->set_angle(this->now, coord::phys_angle_t::from_int(315));
		std::dynamic_pointer_cast<component::Ownership>(entity->get_component(component::component_t::OWNERSHIP))
		    ->set_owner(this->now, player);
		std::dynamic_pointer_cast<component::Activity>(entity->get_component(component::component_t::ACTIVITY))
		    ->init(this->now);
		entity->get_manager()->run_activity_system(this->now);
		this->state->add_game_entity(entity);
		return entity;
	}

	/// a free tile passable for a path grid near a point, nothing if none within \p reach tiles
	std::optional<std::pair<double, double>> free_near(double ne, double se, const std::string &grid_name,
	                                                   int reach = 10) const {
		auto map = this->state->get_map();
		auto grid = map->get_grid_id(grid_name);
		for (int r = 0; r <= reach; ++r) {
			for (int dy = -r; dy <= r; ++dy) {
				for (int dx = -r; dx <= r; ++dx) {
					if (std::max(std::abs(dx), std::abs(dy)) != r) {
						continue;
					}
					coord::tile t{static_cast<coord::tile_t>(std::floor(ne)) + dx, static_cast<coord::tile_t>(std::floor(se)) + dy};
					if (map->is_passable(grid, t)) {
						bool taken = false;
						for (const auto &[id, e] : this->state->get_game_entities()) {
							auto p = this->position(e);
							taken = taken
							        or (std::abs(p.ne.to_double() - (t.ne + 0.5)) < 0.6 and std::abs(p.se.to_double() - (t.se + 0.5)) < 0.6);
							if (taken) {
								break;
							}
						}
						if (not taken) {
							return std::pair{static_cast<double>(t.ne) + 0.5, static_cast<double>(t.se) + 0.5};
						}
					}
				}
			}
		}
		return std::nullopt;
	}

	std::optional<std::pair<double, double>> land_near(double ne, double se, int reach = 10) const {
		return this->free_near(ne, se, "hd_base.data.util.path_type.types.Land", reach);
	}

	std::shared_ptr<GameEntity> unit_near(player_id_t player, const std::string &dir, const std::string &name, double ne,
	                                      double se) {
		auto spot = this->land_near(ne, se);
		return spot ? this->spawn_at(player, ge(dir, name), spot->first, spot->second) : nullptr;
	}

	void set_health(const std::shared_ptr<GameEntity> &e, int64_t health) {
		auto stats = this->combat()->get_stats(e->get_id());
		auto live = std::dynamic_pointer_cast<component::Live>(e->get_component(component::component_t::LIVE));
		live->set_attribute(this->now, stats->health_attribute, health);
		this->combat()->health_changed(e->get_id(), health);
	}

	int64_t health(const std::shared_ptr<GameEntity> &e) const {
		auto stats = this->combat()->get_stats(e->get_id());
		auto live = std::dynamic_pointer_cast<component::Live>(e->get_component(component::component_t::LIVE));
		auto h = live->get_attribute(this->now, stats->health_attribute);
		return h.value_or(-1);
	}

	int64_t max_health(const std::shared_ptr<GameEntity> &e) const {
		return this->combat()->get_stats(e->get_id())->max_health;
	}

	void passive(const std::shared_ptr<GameEntity> &e) {
		this->combat()->set_stance(e->get_id(), combat::stance_t::NO_ATTACK);
	}

	/// a command like the user interface sends it (game.send_command)
	void command(component::command::command_t type, const std::vector<std::shared_ptr<GameEntity>> &units,
	             std::optional<entity_id_t> target, const coord::phys3 &point, int arg = 0) {
		std::vector<entity_id_t> ids;
		for (const auto &u : units) {
			ids.push_back(u->get_id());
		}
		openage::event::EventHandler::param_map::map_t params{
			{"type", type},
			{"target", point},
			{"entity_ids", ids},
			{"combat_arg", arg},
		};
		if (target) {
			params.emplace("target_entity", *target);
		}
		this->loop->create_event("game.send_command", this->commander, this->state,
		                         this->now + time::time_t::from_double(0.001), params);
		this->advance(0.1);
	}

	double resource(player_id_t player, resource_t type) const {
		return this->state->get_player(player)->get_resources().get(type);
	}
};

coord::phys3 at(double ne, double se) {
	return coord::phys3{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}};
}

/// own space for a scene: a free land spot this far from the town centre of player 0
std::pair<double, double> scene_spot(World &w, double dx, double dy) {
	auto tc = w.town_center(0);
	auto p = w.position(tc);
	auto spot = w.land_near(p.ne.to_double() + dx, p.se.to_double() + dy, 12);
	return spot.value_or(std::pair{p.ne.to_double() + dx, p.se.to_double() + dy});
}

MapSettings quiet_map() {
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	return base;
}

// ------------------------------------------------------------------ S1

void check_map(const Options &opt) {
	std::cout << "== S1 Karte: Reliquien" << std::endl;
	for (size_t size : {48u, 64u, 96u}) {
		for (uint32_t seed = 1; seed <= 4; ++seed) {
			MapSettings s;
			s.type = map_type_t::RANDOM;
			s.seed = seed;
			s.size = size;
			auto with = generate_map(s);
			s.relics = false;
			auto without = generate_map(s);
			size_t relics = 0;
			for (const auto &o : with.objects) {
				relics += o.kind == map_object_t::RELIC ? 1 : 0;
			}
			bool prefix = without.objects.size() + relics == with.objects.size() and without.tiles == with.tiles
			              and without.blocked == with.blocked;
			for (size_t i = 0; prefix and i < without.objects.size(); ++i) {
				const auto &a = without.objects[i];
				const auto &c = with.objects[i];
				prefix = a.kind == c.kind and a.ne == c.ne and a.se == c.se and a.owner == c.owner and a.angle == c.angle;
			}
			const std::string what = std::to_string(size) + " #" + std::to_string(seed);
			check(relics == relic_count(with.width), what + ": " + std::to_string(relics) + " relics (relic_count "
			                                             + std::to_string(relic_count(with.width)) + ")");
			check(prefix, what + ": without relics the map of before W6 (relics appended last)");
		}
	}
	World w{opt, quiet_map()};
	w.advance(1.0);
	auto relics = w.named("Relic");
	check(relics.size() == relic_count(map_generator_size(opt.size)), "relics on the game map: " + std::to_string(relics.size()));
	const auto gaia = static_cast<player_id_t>(2);
	bool all_gaia = true;
	bool none_attackable = true;
	for (const auto &r : relics) {
		all_gaia = all_gaia and w.owner(r) == gaia;
		auto stats = w.combat()->get_stats(r->get_id());
		none_attackable = none_attackable and stats and not stats->attackable();
		check(w.special()->is_relic(r->get_id()), "relic " + std::to_string(r->get_id()) + " known to the special state");
	}
	check(all_gaia, "relics belong to gaia");
	check(none_attackable, "relics cannot be attacked (indestructible)");
}

// ------------------------------------------------------------------ S2 – S5

void check_monks(const Options &opt) {
	std::cout << "== S2 Mönch ausbilden, Werte der Daten" << std::endl;
	World w{opt, quiet_map()};
	w.advance(0.5);
	w.production->grant_resources(0, 5000.0);
	w.production->grant_resources(1, 5000.0);
	w.production->grant_research(0, "FeudalAge");
	w.advance(0.5);
	w.production->grant_research(0, "CastleAge");
	w.advance(0.5);
	auto spot = scene_spot(w, 8.0, 0.0);
	auto monastery = w.spawn_at(0, ge("monastery", "Monastery"), spot.first, spot.second);
	w.advance(0.5);
	w.production->set_selection({monastery->get_id()});
	w.advance(0.3);
	auto snap = w.production->snapshot();
	const prod::Option *monk_option = nullptr;
	for (const auto &o : snap.options) {
		if (o.id == "Monk") {
			monk_option = &o;
		}
	}
	check(monk_option != nullptr and monk_option->available,
	      "monastery offers the monk" + (monk_option ? " (" + monk_option->reason + ")" : std::string{}));
	const double train_time = monk_option ? monk_option->time : 51.0;
	w.production->train_in(monastery->get_id(), "Monk");
	check(w.advance_until(train_time + 15.0, [&]() { return not w.of(0, "Monk").empty(); }), "monk trained");
	auto monks = w.of(0, "Monk");
	if (monks.empty()) {
		return;
	}
	auto monk = monks.front();
	w.advance(0.5);
	auto faith = w.special()->faith(monk->get_id());
	check(faith and *faith >= 100.0, "faith 100 at the start (" + fmt(faith.value_or(-1.0)) + ")");
	const auto *t = w.special()->type_of(w.state, monk, w.now);
	check(t and t->monk and t->heals and t->carries_relics, "monk: convert, heal, carry relics (data abilities)");
	if (t) {
		check(std::abs(t->convert_range - 9.0) < 1e-6 and std::abs(t->convert_reload - 1.0) < 1e-6
		          and std::abs(t->convert_chance - 25.0) < 1e-6,
		      "convert: range " + fmt(t->convert_range) + ", reload " + fmt(t->convert_reload) + " s, chance "
		          + fmt(t->convert_chance));
		check(std::abs(t->convert_min - 4.0) < 1e-6 and std::abs(t->convert_max - 10.0) < 1e-6,
		      "convert time window " + fmt(t->convert_min) + "-" + fmt(t->convert_max) + " s (task 104)");
		check(std::abs(t->heal_range - 4.0) < 1e-6 and t->heal_task > 0.0 and t->work_rate > 0.0,
		      "heal: range " + fmt(t->heal_range) + ", a1 " + fmt(t->heal_task) + ", work rate " + fmt(t->work_rate));
		check(t->faith_rate > 0.0, "faith regeneration of the data " + fmt(t->faith_rate) + "/s");
	}

	std::cout << "== S3 Konvertierung" << std::endl;
	auto mp = w.position(monk);
	auto enemy = w.unit_near(1, "militia", "Militia", mp.ne.to_double() + 5.0, mp.se.to_double());
	check(enemy != nullptr, "enemy militia placed");
	if (enemy == nullptr) {
		return;
	}
	w.passive(enemy);
	w.passive(monk);
	w.advance(0.3);
	const double t0 = w.now.to_double();
	// through the command path of the user interface
	w.command(component::command::command_t::CONVERT, {monk}, enemy->get_id(), w.position(enemy));
	check(w.special()->get_job_target(monk->get_id()) == enemy->get_id(), "command 'Konvertieren' gives the job");
	check(w.advance_until(20.0, [&]() { return w.owner(enemy) == 0; }), "militia converted");
	const double took = w.now.to_double() - t0;
	check(took >= 4.0 - 0.3 and took <= 10.0 + 1.5,
	      "conversion inside the window T_min 4 s .. T_max 10 s (+ tick): " + fmt(took) + " s");
	check(not w.combat()->get_target(enemy->get_id()).has_value(), "converted unit has no target");
	auto after = w.special()->faith(monk->get_id()).value_or(-1.0);
	check(after < 1.0, "faith 0 after the conversion (" + fmt(after) + ")");
	w.advance(10.0);
	const double recharged = w.special()->faith(monk->get_id()).value_or(-1.0);
	check(recharged > 14.0 and recharged < 18.0, "faith recharges with 1.6/s: " + fmt(recharged) + " after 10 s");
	// L45: no conversion without full faith
	auto enemy2 = w.unit_near(1, "militia", "Militia", mp.ne.to_double() + 6.0, mp.se.to_double() + 2.0);
	if (enemy2) {
		w.passive(enemy2);
		auto reason = w.special()->order_convert(w.state, monk->get_id(), enemy2->get_id(), w.now);
		check(not reason.empty(), "no conversion with faith " + fmt(recharged) + ": " + reason);
	}
	// Atonement / Redemption
	auto monk2 = w.unit_near(0, "monk", "Monk", mp.ne.to_double() - 2.0, mp.se.to_double());
	auto enemy_monk = w.unit_near(1, "monk", "Monk", mp.ne.to_double() + 4.0, mp.se.to_double() - 3.0);
	auto enemy_ram = w.unit_near(1, "ram", "Ram", mp.ne.to_double() + 4.0, mp.se.to_double() + 3.0);
	w.advance(0.5);
	if (monk2 and enemy_monk and enemy_ram) {
		w.passive(enemy_ram);
		auto r1 = w.special()->order_convert(w.state, monk2->get_id(), enemy_monk->get_id(), w.now);
		check(r1.find("Sühne") != std::string::npos, "monks need Atonement: " + r1);
		auto r2 = w.special()->order_convert(w.state, monk2->get_id(), enemy_ram->get_id(), w.now);
		check(r2.find("Erlösung") != std::string::npos, "siege needs Redemption: " + r2);
		auto tc1 = w.town_center(1);
		auto r3 = tc1 ? w.special()->order_convert(w.state, monk2->get_id(), tc1->get_id(), w.now) : std::string{"-"};
		check(not r3.empty(), "town centre not convertible: " + r3);
		w.production->grant_research(0, "Redemption");
		w.advance(0.5);
		auto r4 = w.special()->order_convert(w.state, monk2->get_id(), enemy_ram->get_id(), w.now);
		check(r4.empty(), "with Redemption the ram can be converted" + (r4.empty() ? std::string{} : ": " + r4));
		w.special()->cancel_job(monk2->get_id());
	}

	std::cout << "== S4 Ketzerei" << std::endl;
	// Heresy is researched in a monastery of the Castle Age
	if (auto tc1 = w.town_center(1)) {
		auto p1 = w.position(tc1);
		auto spot1 = w.land_near(p1.ne.to_double() - 6.0, p1.se.to_double() + 6.0, 12);
		if (spot1) {
			w.spawn_at(1, ge("monastery", "Monastery"), spot1->first, spot1->second);
		}
	}
	w.production->grant_research(1, "FeudalAge");
	w.advance(0.3);
	w.production->grant_research(1, "CastleAge");
	w.advance(0.3);
	w.production->grant_research(1, "Heresy");
	w.advance(0.5);
	{
		bool heresy = false;
		if (const auto *pt = w.state->get_tech()->find(1)) {
			for (const auto &t : pt->order) {
				heresy = heresy or (t.size() >= 7 and t.compare(t.size() - 7, 7, ".Heresy") == 0);
			}
		}
		check(heresy, "player 1 researched Heresy (test grant)");
	}
	auto heretic = w.unit_near(1, "militia", "Militia", mp.ne.to_double() + 3.0, mp.se.to_double() - 2.0);
	if (monk2 and heretic) {
		w.passive(heretic);
		w.advance(0.3);
		auto reason = w.special()->order_convert(w.state, monk2->get_id(), heretic->get_id(), w.now);
		check(reason.empty(), "monk 2 converts the militia of the heretic player" + (reason.empty() ? std::string{} : ": " + reason));
		check(w.advance_until(20.0, [&]() { return not w.alive(heretic); }), "heresy: the converted unit dies");
		check(w.owner(heretic) == 1, "heresy: it dies as player 1's unit");
		check(w.special()->counters().heresy_deaths >= 1, "heresy counted");
	}
	if (monk2) {
		// only one healer for the heal rate below
		w.combat()->kill(w.state, monk2->get_id(), w.now);
		w.advance(0.5);
	}

	std::cout << "== S5 Heilen" << std::endl;
	auto soldier = w.unit_near(0, "militia", "Militia", mp.ne.to_double() - 1.0, mp.se.to_double() + 2.0);
	if (soldier) {
		w.passive(soldier);
		const auto max = w.max_health(soldier);
		w.set_health(soldier, max / 2);
		w.advance(0.3);
		auto reason = w.special()->order_heal(w.state, monk->get_id(), soldier->get_id(), w.now);
		check(reason.empty(), "monk heals the own militia" + (reason.empty() ? std::string{} : ": " + reason));
		const auto h0 = w.health(soldier);
		w.advance(4.0);
		const auto h1 = w.health(soldier);
		const double expected = special::heal_hp(t ? t->work_rate : 1.25, t ? t->heal_task : 2.0, 4.0, 1, 0.0);
		check(h1 > h0 and std::abs(static_cast<double>(h1 - h0) - expected) <= std::max(3.0, expected * 0.5),
		      "heal rate: +" + std::to_string(h1 - h0) + " HP in 4 s (spec " + fmt(expected) + ")");
		check(w.advance_until(60.0, [&]() { return w.health(soldier) >= max; }), "healed to full health");
		if (enemy_ram) {
			auto own_ram = w.unit_near(0, "ram", "Ram", mp.ne.to_double() - 3.0, mp.se.to_double() - 2.0);
			if (own_ram) {
				w.set_health(own_ram, w.max_health(own_ram) / 2);
				auto r = w.special()->order_heal(w.state, monk->get_id(), own_ram->get_id(), w.now);
				check(not r.empty(), "siege not healable: " + r);
			}
		}
		auto r2 = w.special()->order_heal(w.state, monk->get_id(), monastery->get_id(), w.now);
		check(not r2.empty(), "buildings not healable: " + r2);
		// automatic healing (stance preference Heal of the data)
		w.set_health(soldier, max / 2);
		w.combat()->set_stance(monk->get_id(), combat::stance_t::AGGRESSIVE);
		w.special()->cancel_job(monk->get_id());
		w.advance(7.0);
		check(w.advance_until(40.0, [&]() { return w.health(soldier) >= max; }), "idle monk heals the wounded unit on its own");
	}
}

// ------------------------------------------------------------------ S6

void check_relics(const Options &opt) {
	std::cout << "== S6 Reliquien" << std::endl;
	World w{opt, quiet_map()};
	w.advance(0.5);
	w.production->grant_research(0, "FeudalAge");
	w.advance(0.3);
	w.production->grant_research(0, "CastleAge");
	w.advance(0.3);
	auto relics = w.named("Relic");
	check(relics.size() >= 2, "relics on the map (" + std::to_string(relics.size()) + ")");
	if (relics.size() < 2) {
		return;
	}
	auto relic = relics.front();
	auto rp = w.position(relic);
	auto monk = w.unit_near(0, "monk", "Monk", rp.ne.to_double() + 2.0, rp.se.to_double());
	auto mspot = w.land_near(rp.ne.to_double() + 6.0, rp.se.to_double() + 3.0, 12);
	check(monk != nullptr and mspot.has_value(), "monk and monastery spot next to the relic");
	if (monk == nullptr or not mspot) {
		return;
	}
	auto monastery = w.spawn_at(0, ge("monastery", "Monastery"), mspot->first, mspot->second);
	w.passive(monk);
	w.advance(0.5);
	auto reason = w.special()->order_pickup(w.state, monk->get_id(), relic->get_id(), w.now);
	check(reason.empty(), "monk sent to the relic" + (reason.empty() ? std::string{} : ": " + reason));
	check(w.advance_until(30.0, [&]() { return w.special()->carried_relic(monk->get_id()).has_value(); }),
	      "monk picks the relic up");
	check(w.combat()->garrisoned_in(relic->get_id()).has_value(), "carried relic is hidden");
	auto info = w.special()->get_info(monk->get_id());
	check(info and info->relic, "HUD info: monk carries a relic");
	// the graphics follow the relic; the game keeps the original paths
	const auto carries = [](const std::shared_ptr<GameEntity> &e, const std::string &part) {
		return e->get_shown_animation_path().find(part) != std::string::npos;
	};
	const auto *mt = w.special()->type_of(w.state, monk, w.now);
	check(mt and mt->carry_idle_animation.find("idle_carry_override_monk") != std::string::npos
	          and mt->carry_move_animation.find("move_carry_override_monk") != std::string::npos,
	      "monk: carry graphics from the data");
	std::cout << "   shown after the pick-up: " << monk->get_shown_animation_path() << std::endl;
	check(carries(monk, "carry_override_monk"), "carry graphics right after the pick-up");
	check(monk->get_animation_path().find("carry") == std::string::npos, "the game keeps the normal path");
	reason = w.special()->order_deliver(w.state, monk->get_id(), monastery->get_id(), w.now);
	check(reason.empty(), "monk sent to the monastery" + (reason.empty() ? std::string{} : ": " + reason));
	w.advance(0.5);
	check(carries(monk, "move_carry_override_monk"), "walks with the relic: " + monk->get_shown_animation_path());
	check(w.advance_until(40.0, [&]() { return w.special()->stowed(monastery->get_id()).size() == 1; }),
	      "relic in the monastery");
	check(not w.special()->carried_relic(monk->get_id()), "monk hands the relic over");
	check(not carries(monk, "carry_override_monk"), "normal graphics after the hand-over: " + monk->get_shown_animation_path());
	w.advance(1.5);
	const double g0 = w.resource(0, resource_t::GOLD);
	w.advance(20.0);
	const double gained = w.resource(0, resource_t::GOLD) - g0;
	check(std::abs(gained - 10.0) <= 0.6, "relic gold 0.5/s (02 §2.10): +" + fmt(gained) + " gold in 20 s");
	auto minfo = w.special()->get_info(monastery->get_id());
	check(minfo and minfo->relics == 1 and std::abs(minfo->relic_gold - 0.5) < 1e-6, "HUD info: monastery with one relic");

	// the carrier dies: the relic falls
	auto second = relics[1];
	auto sp = w.position(second);
	auto monk2 = w.unit_near(0, "monk", "Monk", sp.ne.to_double() + 1.5, sp.se.to_double());
	if (monk2) {
		w.passive(monk2);
		w.special()->order_pickup(w.state, monk2->get_id(), second->get_id(), w.now);
		check(w.advance_until(30.0, [&]() { return w.special()->carried_relic(monk2->get_id()).has_value(); }),
		      "second monk carries a relic");
		check(carries(monk2, "carry_override_monk"), "second monk shows the carry graphics");
		// put down by hand, picked up again
		reason = w.special()->drop_relic(w.state, monk2->get_id(), w.now);
		check(reason.empty() and not carries(monk2, "carry_override_monk"), "normal graphics after putting it down");
		w.special()->order_pickup(w.state, monk2->get_id(), second->get_id(), w.now);
		check(w.advance_until(30.0, [&]() { return w.special()->carried_relic(monk2->get_id()).has_value(); })
		          and carries(monk2, "carry_override_monk"),
		      "carry graphics again after the second pick-up");
		w.combat()->kill(w.state, monk2->get_id(), w.now);
		w.advance(0.5);
		check(not w.combat()->garrisoned_in(second->get_id()).has_value() and w.state->get_game_entities().contains(second->get_id()),
		      "relic falls to the ground when its carrier dies");
		check(w.special()->counters().relics_dropped >= 1, "dropped relic counted");
	}
}

// ------------------------------------------------------------------ S7

void check_siege(const Options &opt) {
	std::cout << "== S7 Belagerung: Mange, Rammbock, Tribock" << std::endl;
	World w{opt, quiet_map()};
	w.advance(0.5);
	auto c = scene_spot(w, 10.0, -6.0);
	// mangonel: cluster of enemy militia with an own militia next to the target
	auto target = w.unit_near(1, "militia", "Militia", c.first, c.second);
	if (target == nullptr) {
		check(false, "scene for the mangonel");
		return;
	}
	auto tp = w.position(target);
	const double x = tp.ne.to_double();
	const double y = tp.se.to_double();
	std::vector<std::shared_ptr<GameEntity>> enemies{target};
	for (auto off : {std::pair{0.6, 0.0}, std::pair{0.0, 0.6}}) {
		auto e = w.spawn_at(1, ge("militia", "Militia"), x + off.first, y + off.second);
		enemies.push_back(e);
	}
	auto own = w.spawn_at(0, ge("militia", "Militia"), x - 0.6, y);
	for (const auto &e : enemies) {
		w.passive(e);
	}
	w.passive(own);
	auto mangonel = w.unit_near(0, "mangonel", "Mangonel", x - 5.0, y);
	check(mangonel != nullptr, "mangonel placed 5 tiles away");
	if (mangonel == nullptr) {
		return;
	}
	const auto *mt = w.special()->type_of(w.state, mangonel, w.now);
	check(mt and mt->blast_range > 0.0, "mangonel has an area radius in the data (" + fmt(mt ? mt->blast_range : 0.0) + ")");
	w.advance(0.3);
	const auto own_h0 = w.health(own);
	w.combat()->order_attack(w.state, mangonel->get_id(), target->get_id(), w.now);
	bool splash = w.advance_until(60.0, [&]() {
		return w.special()->counters().splash_hits >= 1 and w.special()->counters().friendly_hits >= 1;
	});
	auto counters = w.special()->counters();
	check(splash, "mangonel shot hits units around the target: " + std::to_string(counters.splash_hits) + " area hits, "
	                  + std::to_string(counters.friendly_hits) + " own");
	check(w.health(own) < own_h0 or not w.alive(own), "own militia next to the target took damage (mangonel hits own units)");

	// ram: buildings only
	auto c2 = scene_spot(w, -10.0, 6.0);
	auto house = w.spawn_at(1, ge("house", "House"), std::floor(c2.first) + 1.0, std::floor(c2.second) + 1.0);
	auto guard = w.unit_near(1, "militia", "Militia", c2.first + 2.5, c2.second);
	// some tiles away: the ram walks there, also if the side next to it is blocked
	auto ram = w.spawn_at(0, ge("ram", "Ram"), std::floor(c2.first) + 6.0, std::floor(c2.second) + 1.0);
	if (house == nullptr or guard == nullptr or ram == nullptr) {
		check(false, "scene for the ram");
		return;
	}
	w.passive(guard);
	w.advance(0.3);
	const auto house_h0 = w.health(house);
	w.combat()->order_attack(w.state, ram->get_id(), house->get_id(), w.now);
	const bool rammed = w.advance_until(60.0, [&]() { return w.health(house) < house_h0; });
	std::cout << "  ram at " << fmt(w.distance(ram, house)) << " tiles from the house, target "
	          << (w.combat()->get_target(ram->get_id()) ? std::to_string(*w.combat()->get_target(ram->get_id())) : std::string{"-"})
	          << ", house " << w.health(house) << "/" << w.max_health(house) << std::endl;
	check(rammed, "ram damages the house");
	w.combat()->stop(w.state, ram->get_id(), w.now);
	w.advance(0.3);
	bool ram_on_unit = false;
	w.command(component::command::command_t::ATTACK, {ram}, guard->get_id(), w.position(guard));
	w.advance_until(10.0, [&]() {
		ram_on_unit = ram_on_unit or w.combat()->get_target(ram->get_id()) == guard->get_id();
		return false;
	});
	check(not ram_on_unit, "ram does not take a unit as target (ordered or on its own)");

	// repair of siege units by villagers (spec 04 §4.5): a quarter of the building rate, costs of the owner
	{
		w.production->grant_resources(0, 2000.0);
		auto c4 = scene_spot(w, 6.0, 10.0);
		auto wreck = w.unit_near(0, "ram", "Ram", c4.first, c4.second);
		auto worker = w.unit_near(0, "villager", "Villager", c4.first + 2.0, c4.second);
		if (wreck == nullptr or worker == nullptr) {
			check(false, "scene for the repair");
			return;
		}
		w.passive(wreck);
		const auto max = w.max_health(wreck);
		w.set_health(wreck, max / 2);
		w.advance(0.3);
		const auto wood0 = w.resource(0, resource_t::WOOD);
		const auto gold0 = w.resource(0, resource_t::GOLD);
		w.command(component::command::command_t::REPAIR, {worker}, wreck->get_id(), w.position(wreck));
		check(w.advance_until(30.0, [&]() { return w.health(wreck) > max / 2; }), "villager repairs the ram");
		const auto h1 = w.health(wreck);
		w.advance(8.0);
		const auto gain = w.health(wreck) - h1;
		std::cout << "  repair: " << gain << " HP in 8 s, wood " << wood0 - w.resource(0, resource_t::WOOD) << ", gold "
		          << gold0 - w.resource(0, resource_t::GOLD) << std::endl;
		check(gain >= 22 and gain <= 28, "one villager repairs a unit at 12.5 / 4 HP/s: " + std::to_string(gain));
		check(wood0 - w.resource(0, resource_t::WOOD) + gold0 - w.resource(0, resource_t::GOLD) > 0.0,
		      "the repair costs resources");
		w.set_health(wreck, max);
		w.advance(1.0);
		check(not w.special()->order_repair(w.state, worker->get_id(), wreck->get_id(), w.now).empty(),
		      "an undamaged unit is no repair target");
		check(not w.special()->order_repair(w.state, worker->get_id(), guard->get_id(), w.now).empty(),
		      "an enemy unit is no repair target");
		// nothing to pay: the repair stops
		auto &stock = w.state->get_player(0)->get_resources();
		stock.spend(stock.get());
		w.set_health(wreck, max / 2);
		w.advance(0.3);
		check(w.special()->order_repair(w.state, worker->get_id(), wreck->get_id(), w.now).empty(),
		      "repair ordered without resources");
		w.advance(20.0);
		check(w.health(wreck) < max / 2 + 15, "the repair stops if the owner cannot pay: " + std::to_string(w.health(wreck)));
	}

	// trebuchet: unpack / pack (transform unit of the data)
	auto c3 = scene_spot(w, -6.0, -10.0);
	auto treb = w.unit_near(0, "trebuchet", "Trebuchet", c3.first, c3.second);
	if (treb == nullptr) {
		check(false, "trebuchet placed");
		return;
	}
	w.advance(0.5);
	const auto *tt = w.special()->type_of(w.state, treb, w.now);
	check(tt and tt->packable, "trebuchet can pack (transform unit of the data)");
	check(tt and tt->unpacked_idle_animation.find("idle_form_override_trebuchet") != std::string::npos,
	      "the unpacked form has its own standing graphic in the data: " + (tt ? tt->unpacked_idle_animation : std::string{}));
	const auto stands_unpacked = [&]() {
		return treb->get_shown_animation_path().find("idle_form_override_trebuchet") != std::string::npos;
	};
	check(not stands_unpacked(), "packed graphic before unpacking: " + treb->get_shown_animation_path());
	auto r = w.special()->set_packed(w.state, treb->get_id(), false, w.now);
	check(r.empty(), "trebuchet unpacks" + (r.empty() ? std::string{} : ": " + r));
	w.advance(1.0);
	check(not stands_unpacked(), "still the packed graphic while it unpacks");
	check(w.advance_until(special::SpecialState::PACK_TIME + 1.0, [&]() { return w.special()->is_unpacked(treb->get_id()); }),
	      "unpacked after " + fmt(special::SpecialState::PACK_TIME) + " s");
	w.advance(0.2);
	check(stands_unpacked(), "stands with the unpacked graphic: " + treb->get_shown_animation_path());
	auto p0 = w.position(treb);
	auto dest = w.land_near(p0.ne.to_double() + 4.0, p0.se.to_double(), 8);
	if (dest) {
		w.command(component::command::command_t::MOVE, {treb}, std::nullopt, at(dest->first, dest->second));
		w.advance(2.0);
		auto p1 = w.position(treb);
		check(std::hypot(p1.ne.to_double() - p0.ne.to_double(), p1.se.to_double() - p0.se.to_double()) < 0.05,
		      "unpacked trebuchet does not move (it packs first)");
		check(w.advance_until(special::SpecialState::PACK_TIME + 12.0, [&]() {
			auto p = w.position(treb);
			return std::hypot(p.ne.to_double() - p0.ne.to_double(), p.se.to_double() - p0.se.to_double()) > 1.0;
		}),
		      "after packing it moves");
		check(not w.special()->is_unpacked(treb->get_id()), "trebuchet packed");
		check(not stands_unpacked(), "packed graphic again after packing: " + treb->get_shown_animation_path());
	}
	// it dies with the graphic of its form
	const auto dies_unpacked = [&]() {
		return treb->get_shown_animation_path().find("death_form_override_trebuchet") != std::string::npos;
	};
	check(tt and tt->unpacked_death_animation.find("death_form_override_trebuchet") != std::string::npos,
	      "the unpacked form has its own death graphic in the data: " + (tt ? tt->unpacked_death_animation : std::string{}));
	r = w.special()->set_packed(w.state, treb->get_id(), false, w.now);
	check(r.empty(), "trebuchet unpacks again" + (r.empty() ? std::string{} : ": " + r));
	check(w.advance_until(special::SpecialState::PACK_TIME + 1.0, [&]() { return w.special()->is_unpacked(treb->get_id()); }),
	      "unpacked again");
	w.advance(0.2);
	w.combat()->kill(w.state, treb->get_id(), w.now);
	w.advance(0.01);
	check(dies_unpacked(), "dies with the unpacked graphic: " + treb->get_shown_animation_path());
}

// ------------------------------------------------------------------ S8

struct Shore {
	double water_ne;
	double water_se;
	double land_ne;
	double land_se;
};

std::optional<Shore> find_shore(World &w) {
	auto map = w.state->get_map();
	auto ship_grid = map->get_grid_id("hd_base.data.util.path_type.types.Water");
	auto land_grid = map->get_grid_id("hd_base.data.util.path_type.types.Land");
	auto tc = w.town_center(0);
	auto tp = w.position(tc);
	std::optional<Shore> best;
	double best_d = 1e18;
	const auto size = map->get_size();
	for (long y = 2; y < static_cast<long>(size[1]) - 2; ++y) {
		for (long x = 2; x < static_cast<long>(size[0]) - 2; ++x) {
			if (not map->is_passable(ship_grid, coord::tile{x, y})) {
				continue;
			}
			// deep enough: water all around, land two tiles away
			bool open = true;
			for (long dy = -1; dy <= 1 and open; ++dy) {
				for (long dx = -1; dx <= 1 and open; ++dx) {
					open = map->is_passable(ship_grid, coord::tile{x + dx, y + dy});
				}
			}
			if (not open) {
				continue;
			}
			for (auto off : {std::pair{2L, 0L}, std::pair{-2L, 0L}, std::pair{0L, 2L}, std::pair{0L, -2L}}) {
				coord::tile l{x + off.first, y + off.second};
				if (not map->is_passable(land_grid, l)) {
					continue;
				}
				const double d = std::hypot(x - tp.ne.to_double(), y - tp.se.to_double());
				if (d < best_d) {
					best_d = d;
					best = Shore{x + 0.5, y + 0.5, l.ne + 0.5, l.se + 0.5};
				}
			}
		}
	}
	return best;
}

void check_ships(const Options &opt) {
	std::cout << "== S8 Schiffe: Transport, Galeere (Küste)" << std::endl;
	MapSettings base = quiet_map();
	base.biome = map_biome_t::COAST;
	World w{opt, base, opt.coast_seed};
	w.advance(0.5);
	auto shore = find_shore(w);
	check(shore.has_value(), "coast: water next to land");
	if (not shore) {
		return;
	}
	auto ship = w.spawn_at(0, ge("transport_ship", "TransportShip"), shore->water_ne, shore->water_se);
	w.advance(0.5);
	const auto *st = w.special()->type_of(w.state, ship, w.now);
	check(st and st->cargo == special::cargo_t::TRANSPORT and st->cargo_slots == 5,
	      "transport ship takes " + std::to_string(st ? st->cargo_slots : 0) + " land units (data)");
	std::vector<std::shared_ptr<GameEntity>> soldiers;
	for (int i = 0; i < 3; ++i) {
		auto s = w.unit_near(0, "militia", "Militia", shore->land_ne, shore->land_se);
		if (s) {
			w.passive(s);
			soldiers.push_back(s);
		}
	}
	check(soldiers.size() == 3, "three militia at the shore");
	// a right click on the own ship boards
	w.command(component::command::command_t::MOVE, soldiers, ship->get_id(), w.position(ship));
	check(w.advance_until(60.0, [&]() { return w.special()->stowed(ship->get_id()).size() == soldiers.size(); }),
	      "militia board the ship (" + std::to_string(w.special()->stowed(ship->get_id()).size()) + ")");
	bool hidden = true;
	for (const auto &s : soldiers) {
		hidden = hidden and w.combat()->garrisoned_in(s->get_id()) == ship->get_id();
	}
	check(hidden, "units on board are hidden");
	auto info = w.special()->get_info(ship->get_id());
	check(info and info->cargo == 3 and info->cargo_max == 5 and info->transport, "HUD info: cargo 3/5");

	// save with cargo (version 6)
	auto data = save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "W6");
	const auto text = save::serialize(data);
	check(text.find("cargo = ") != std::string::npos, "cargo stored in the save");

	// unload at the shore
	w.command(component::command::command_t::UNLOAD, {ship}, std::nullopt, at(shore->land_ne, shore->land_se));
	check(w.advance_until(60.0, [&]() { return w.special()->stowed(ship->get_id()).empty(); }), "ship unloads at the shore");
	auto land_grid = w.state->get_map()->get_grid_id("hd_base.data.util.path_type.types.Land");
	bool on_land = true;
	for (const auto &s : soldiers) {
		on_land = on_land and w.state->get_map()->is_passable(land_grid, w.position(s).to_tile())
		          and not w.combat()->garrisoned_in(s->get_id());
	}
	check(on_land, "unloaded units stand on land");

	// load again and sink the ship: the cargo is lost
	w.command(component::command::command_t::MOVE, {soldiers[0]}, ship->get_id(), w.position(ship));
	w.advance_until(40.0, [&]() { return w.special()->stowed(ship->get_id()).size() == 1; });
	w.combat()->kill(w.state, ship->get_id(), w.now);
	w.advance(1.0);
	check(not w.alive(soldiers[0]) and w.special()->counters().cargo_lost >= 1, "cargo of a sunken transport is lost");

	// the dock trains galleys (SHIPS free)
	auto dock = w.spawn_at(0, ge("dock", "Dock"), shore->water_ne, shore->water_se);
	w.production->grant_research(0, "FeudalAge");
	w.production->grant_resources(0, 2000.0);
	w.advance(0.5);
	w.production->set_selection({dock->get_id()});
	w.advance(0.3);
	auto snap = w.production->snapshot();
	bool galley = false;
	for (const auto &o : snap.options) {
		galley = galley or (o.id == "Galley" and o.available);
	}
	check(galley, "dock trains galleys in the Feudal Age");

	// repair of a ship from the shore, right click like the user interface (spec 04 §4.5)
	{
		auto hull = w.spawn_at(0, ge("galley", "Galley"), shore->water_ne, shore->water_se);
		auto worker = w.unit_near(0, "villager", "Villager", shore->land_ne, shore->land_se);
		if (hull == nullptr or worker == nullptr) {
			check(false, "scene for the ship repair");
		}
		else {
			w.passive(hull);
			const auto max = w.max_health(hull);
			w.set_health(hull, max / 2);
			w.advance(0.3);
			w.command(component::command::command_t::MOVE, {worker}, hull->get_id(), w.position(hull));
			check(w.advance_until(30.0, [&]() { return w.health(hull) > max / 2; }), "villager at the shore repairs the ship");
		}
	}

	// restore the cargo from the save
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parse save with cargo " + error);
	World l{opt, base, opt.coast_seed};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	check(result.ok, "restored " + result.error);
	l.advance(0.5);
	auto ships = l.of(0, "TransportShip");
	check(ships.size() == 1 and l.special()->stowed(ships.front()->get_id()).size() == 3, "cargo restored on the ship");
}

// ------------------------------------------------------------------ S9

/// S11: what the first scenes do not reach (warships, demolition ship, petard, ram with infantry)
void check_gaps(const Options &opt) {
	std::cout << "== S11 Kriegsschiffe, Sprengschiff, Petarde, Rammbock mit Infanterie" << std::endl;
	MapSettings base = quiet_map();
	base.biome = map_biome_t::COAST;
	World w{opt, base, opt.coast_seed};
	w.advance(0.5);
	auto shore = find_shore(w);
	check(shore.has_value(), "coast for the warships");
	if (not shore) {
		return;
	}
	const std::string water = "hd_base.data.util.path_type.types.Water";
	auto spot_a = w.free_near(shore->water_ne, shore->water_se, water, 3);
	auto spot_b = w.free_near(shore->water_ne + 4.0, shore->water_se, water, 8);
	auto spot_c = w.free_near(shore->water_ne, shore->water_se + 4.0, water, 8);
	if (not spot_a or not spot_b or not spot_c) {
		check(false, "water for the warships");
		return;
	}
	// warships fight with the combat logic on the water grid
	auto own_galley = w.spawn_at(0, ge("galley", "Galley"), spot_a->first, spot_a->second);
	auto enemy_galley = w.spawn_at(1, ge("galley", "Galley"), spot_b->first, spot_b->second);
	w.passive(enemy_galley);
	w.advance(0.5);
	const auto galley_h0 = w.health(enemy_galley);
	w.combat()->order_attack(w.state, own_galley->get_id(), enemy_galley->get_id(), w.now);
	check(w.advance_until(40.0, [&]() { return not w.alive(enemy_galley) or w.health(enemy_galley) < galley_h0; }),
	      "a galley hits an enemy galley");
	w.combat()->stop(w.state, own_galley->get_id(), w.now);
	// demolition ship: area damage, destroys itself
	auto victim = w.spawn_at(1, ge("galley", "Galley"), spot_c->first, spot_c->second);
	auto demo = w.spawn_at(0, ge("demo_ship", "DemolitionShip"), spot_a->first, spot_a->second);
	w.passive(victim);
	w.advance(0.5);
	const auto *dt = w.special()->type_of(w.state, demo, w.now);
	check(dt and dt->self_destruct and dt->blast_range > 0.0, "demolition ship: area attack, destroys itself");
	const auto victim_h0 = w.health(victim);
	w.combat()->order_attack(w.state, demo->get_id(), victim->get_id(), w.now);
	check(w.advance_until(60.0, [&]() { return not w.alive(demo); }), "demolition ship destroys itself");
	check(not w.alive(victim) or w.health(victim) < victim_h0, "demolition ship damages its target");

	// petard: goes to the building, destroys itself
	auto c1 = scene_spot(w, 10.0, -6.0);
	auto house = w.spawn_at(1, ge("house", "House"), std::floor(c1.first) + 1.0, std::floor(c1.second) + 1.0);
	auto petard = w.unit_near(0, "petard", "Petard", c1.first + 5.0, c1.second);
	if (house == nullptr or petard == nullptr) {
		check(false, "scene for the petard");
		return;
	}
	w.advance(0.5);
	const auto house_h0 = w.health(house);
	w.combat()->order_attack(w.state, petard->get_id(), house->get_id(), w.now);
	check(w.advance_until(60.0, [&]() { return not w.alive(petard); }), "petard destroys itself");
	check(not w.alive(house) or w.health(house) < house_h0, "petard damages the building");

	// ram with infantry: boards by right click, archers are refused, unload
	auto c2 = scene_spot(w, -8.0, 8.0);
	auto ram = w.unit_near(0, "ram", "Ram", c2.first, c2.second);
	auto soldier = w.unit_near(0, "militia", "Militia", c2.first + 2.0, c2.second);
	auto archer = w.unit_near(0, "archer", "Archer", c2.first, c2.second + 2.0);
	if (ram == nullptr or soldier == nullptr or archer == nullptr) {
		check(false, "scene for the ram with infantry");
		return;
	}
	w.passive(ram);
	w.passive(soldier);
	w.passive(archer);
	w.advance(0.5);
	check(not w.special()->order_board(w.state, archer->get_id(), ram->get_id(), w.now).empty(),
	      "an archer does not board the ram");
	w.command(component::command::command_t::MOVE, {soldier}, ram->get_id(), w.position(ram));
	check(w.advance_until(30.0, [&]() { return w.special()->stowed(ram->get_id()).size() == 1; }),
	      "infantry boards the ram");
	check(w.special()->unload(w.state, ram->get_id(), std::nullopt, w.now).empty(), "unload ordered");
	check(w.advance_until(20.0, [&]() { return w.special()->stowed(ram->get_id()).empty(); }) and w.alive(soldier),
	      "the ram unloads the infantry");
}

/// S13: the trebuchet shoots (projectile of its unpacked form), unpacks and packs for it
void check_trebuchet_fire(const Options &opt) {
	std::cout << "== S13 Tribock schießt" << std::endl;
	World w{opt, quiet_map()};
	w.advance(0.5);
	// everything towards the middle of the map (64 x 64)
	const auto tc = w.position(w.town_center(0));
	const double dne = tc.ne.to_double() < 32.0 ? 1.0 : -1.0;
	const double dse = tc.se.to_double() < 32.0 ? 1.0 : -1.0;
	auto c = scene_spot(w, 6.0 * dne, 0.0);
	auto treb = w.unit_near(0, "trebuchet", "Trebuchet", c.first, c.second);
	auto militia = w.unit_near(1, "militia", "Militia", c.first, c.second + 6.0 * dse);
	auto near_spot = w.land_near(c.first + 22.0 * dne, c.second, 4);
	if (treb == nullptr or militia == nullptr or not near_spot) {
		check(false, "scene for the trebuchet");
		return;
	}
	auto house = w.spawn_at(1, ge("house", "House"), std::floor(near_spot->first) + 1.0, std::floor(near_spot->second) + 1.0);
	if (house == nullptr) {
		check(false, "house for the trebuchet");
		return;
	}
	w.passive(militia);
	w.advance(0.5);
	const auto stats = w.combat()->get_stats(treb->get_id());
	check(stats != nullptr and stats->can_attack and stats->ranged and stats->packs,
	      "the trebuchet can shoot (data of its unpacked form)");
	if (stats == nullptr or not stats->can_attack) {
		return;
	}
	std::cout << "  trebuchet: range " << stats->min_range << " to " << stats->max_range << ", reload "
	          << stats->reload_time << " s" << std::endl;

	// packed: no automatic target, it stays packed next to an enemy
	w.advance(8.0);
	check(not w.special()->is_unpacked(treb->get_id()) and w.distance(treb, militia) < 8.0,
	      "a packed trebuchet does not unpack on its own");

	// the order: too far away, so it walks, unpacks in range and hits
	const auto h0 = w.health(house);
	const auto d0 = w.distance(treb, house);
	w.combat()->order_attack(w.state, treb->get_id(), house->get_id(), w.now);
	check(w.advance_until(40.0, [&]() { return w.distance(treb, house) < d0 - 0.5; }),
	      "a far target: the packed trebuchet walks");
	check(w.advance_until(120.0, [&]() { return w.special()->is_unpacked(treb->get_id()); }), "in range it unpacks");
	const auto spot = w.position(treb);
	check(w.advance_until(120.0, [&]() { return not w.alive(house) or w.health(house) <= h0 - 50; }),
	      "the trebuchet hits the building hard (its own attack values, at least 50)");
	std::cout << "  house: " << h0 << " -> " << (w.alive(house) ? w.health(house) : 0) << std::endl;
	const auto now_at = w.position(treb);
	check(std::hypot(now_at.ne.to_double() - spot.ne.to_double(), now_at.se.to_double() - spot.se.to_double()) < 0.01,
	      "unpacked it does not move");

	// a right click on the enemy while it stands unpacked: it stays and goes on shooting
	if (w.alive(house)) {
		w.combat()->stop(w.state, treb->get_id(), w.now);
		const auto h1 = w.health(house);
		w.command(component::command::command_t::MOVE, {treb}, house->get_id(), w.position(house));
		w.advance(special::SpecialState::PACK_TIME + 0.5);
		check(w.special()->is_unpacked(treb->get_id()), "a right click on an enemy in range: it stays unpacked");
		check(w.advance_until(40.0, [&]() { return not w.alive(house) or w.health(house) < h1; }),
		      "and it hits again after the right click");
	}
	else {
		check(false, "the house survives the first hit");
	}

	// a target out of range: it packs first
	w.combat()->stop(w.state, treb->get_id(), w.now);
	auto far_spot = w.land_near(spot.ne.to_double(), spot.se.to_double() + 22.0 * dse, 4);
	if (not far_spot) {
		check(false, "scene for the far target");
		return;
	}
	auto far = w.spawn_at(1, ge("house", "House"), std::floor(far_spot->first) + 1.0, std::floor(far_spot->second) + 1.0);
	w.advance(0.5);
	w.combat()->order_attack(w.state, treb->get_id(), far->get_id(), w.now);
	check(w.advance_until(10.0, [&]() { return not w.special()->is_unpacked(treb->get_id()); }),
	      "a target out of range: it packs first");
	w.combat()->kill(w.state, treb->get_id(), w.now);
	w.advance(0.01);
	const auto packed_death = treb->get_shown_animation_path();
	check(packed_death.find("death_trebuchet") != std::string::npos and packed_death.find("form_override") == std::string::npos,
	      "packed it dies with the packed graphic: " + packed_death);
}

/// S14: the market of W7 (spec 02 §2.7): prices shared by all players, the sale pays the new price, saved
void check_market(const Options &opt) {
	std::cout << "== S14 Markt" << std::endl;
	using component::command::command_t;
	World w{opt, quiet_map()};
	w.advance(0.5);
	auto spot = scene_spot(w, 8.0, -8.0);
	auto market = w.spawn_at(0, ge("market", "Market"), spot.first, spot.second);
	auto house = w.spawn_at(0, ge("house", "House"), spot.first + 6.0, spot.second);
	if (market == nullptr or house == nullptr) {
		check(false, "market for the check");
		return;
	}
	w.advance(0.5);
	const auto info = w.special()->get_info(market->get_id());
	check(info and info->market, "the HUD knows the finished market");
	check(info and info->market_buy[0] == 130 and info->market_sell[0] == 68, "food at the base price: buy 130, sell 68");
	check(info and info->market_buy[2] == 169, "stone at its base price 1.3: buy 169");

	auto stock = [&](player_id_t p, resource_t r) { return w.resource(p, r); };
	auto trade = [&](int arg) {
		w.command(command_t::MARKET, {market}, std::nullopt, w.position(market), arg);
	};
	w.state->get_player(0)->get_resources().set({500.0, 500.0, 500.0, 500.0});
	const double gold0 = stock(0, resource_t::GOLD);
	const double food0 = stock(0, resource_t::FOOD);
	trade(0);
	check(stock(0, resource_t::GOLD) == gold0 - 130 and stock(0, resource_t::FOOD) == food0 + 100, "buy 100 food for 130 gold");
	w.advance(0.3);
	const auto after = w.special()->get_info(market->get_id());
	check(after and after->market_buy[0] == 134, "the next lot costs more: 134");
	check(after and after->market_buy[1] == 130, "wood keeps its price");
	check(w.special()->market_buy_cost(0, 0.3) == 134, "the price is the same for every player");

	// the sale brings the net counter back to 0: base price, and it pays the new price
	const double gold1 = stock(0, resource_t::GOLD);
	trade(1);
	check(stock(0, resource_t::GOLD) == gold1 + 70 and stock(0, resource_t::FOOD) == food0, "sell 100 food for 70 gold at the base price");
	w.advance(0.3);
	check(w.special()->market_buy_cost(0, 0.3) == 130, "net 0: food back at its base price");

	// a sale below the base price, then not enough stone or gold
	const double gold2 = stock(0, resource_t::GOLD);
	trade(3);
	check(stock(0, resource_t::GOLD) == gold2 + 68, "sell wood for 68 gold (new price 0.97)");
	w.state->get_player(0)->get_resources().set({0.0, 0.0, 10.0, 50.0});
	trade(4);
	check(stock(0, resource_t::STONE) == 50.0 and stock(0, resource_t::GOLD) == 10.0, "too little gold: nothing happens");
	trade(5);
	check(stock(0, resource_t::STONE) == 50.0 and stock(0, resource_t::GOLD) == 10.0, "too little stone: nothing happens");
	w.advance(0.3);
	const auto poor = w.special()->get_info(market->get_id());
	check(poor and not poor->market_can_buy[2] and not poor->market_can_sell[2], "the HUD disables both");

	// only a market trades: a house, a foundation or a foreign building do nothing
	w.state->get_player(0)->get_resources().set({500.0, 500.0, 500.0, 500.0});
	w.command(command_t::MARKET, {house}, std::nullopt, w.position(house), 0);
	check(stock(0, resource_t::GOLD) == 500.0, "a house does not trade");
	check(not w.special()->get_info(house->get_id()) or not w.special()->get_info(house->get_id())->market, "the house is no market");

	// tribute (spec 02 §2.8): the fee comes on top, a small stock shrinks both, the techs lower the fee
	auto tribute = [&](int resource, int amount, int to) {
		w.command(command_t::MARKET, {market}, std::nullopt, w.position(market), 100 + resource + 10 * amount + 100 * to);
	};
	w.state->get_player(0)->get_resources().set({500.0, 500.0, 300.0, 500.0});
	w.state->get_player(1)->get_resources().set({0.0, 0.0, 0.0, 0.0});
	tribute(0, 0, 1);
	check(stock(0, resource_t::FOOD) == 370.0 and stock(1, resource_t::FOOD) == 100.0, "tribute 100 food: 130 from the sender, 100 arrive");
	tribute(2, 1, 1);
	check(stock(0, resource_t::GOLD) == 0.0 and stock(1, resource_t::GOLD) == 210.0, "tribute 500 gold from 300: 210 arrive, the stock is empty");
	tribute(2, 0, 1);
	check(stock(1, resource_t::GOLD) == 210.0, "an empty stock sends nothing");
	tribute(1, 2, 1);
	check(stock(0, resource_t::WOOD) == 0.0 and stock(1, resource_t::WOOD) == 350.0, "tribute of the whole stock: 70 percent arrive");
	tribute(3, 0, 0);
	check(stock(0, resource_t::STONE) == 500.0, "no tribute to yourself");
	w.production->grant_research(0, "Coinage");
	w.advance(0.3);
	w.state->get_player(0)->get_resources().set({500.0, 500.0, 300.0, 500.0});
	tribute(0, 0, 1);
	check(stock(0, resource_t::FOOD) == 380.0, "coinage: fee 20 percent");
	w.production->grant_research(0, "Banking");
	w.advance(0.3);
	tribute(0, 0, 1);
	check(stock(0, resource_t::FOOD) == 280.0, "banking: no fee");
	w.production->grant_research(0, "Guilds");
	w.advance(0.3);
	check(w.special()->market_fee(w.state, 0) == 0.15, "guilds: market fee 15 percent");
	const auto after_guilds = w.special()->get_info(market->get_id());
	check(after_guilds and not after_guilds->tribute_to.empty() and after_guilds->tribute_to.front() == 1, "the HUD offers player 2");

	// the prices come back with the save game
	const auto text = save::serialize(save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "W7"));
	check(text.find("market = ") != std::string::npos, "the save game has the prices");
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parsed " + error);
	World l{opt, quiet_map()};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	check(result.ok, "restored " + result.error);
	l.advance(0.5);
	check(l.special()->market_sell_revenue(1, 0.3) == w.special()->market_sell_revenue(1, 0.3), "wood price after loading");
}

/// S15: trade carts shuttle between two own markets, the caravan makes the trips faster (spec 02 §2.9)
void check_trade(const Options &opt) {
	std::cout << "== S15 Handel" << std::endl;
	using component::command::command_t;
	World w{opt, quiet_map()};
	w.advance(0.5);
	auto spot = scene_spot(w, 8.0, -8.0);
	auto home = w.spawn_at(0, ge("market", "Market"), spot.first, spot.second);
	// along the river of the map, on the same bank
	const auto far_spot = w.land_near(spot.first + 8.0, spot.second + 14.0, 8).value_or(std::pair{spot.first + 8.0, spot.second + 14.0});
	auto far = w.spawn_at(0, ge("market", "Market"), far_spot.first, far_spot.second);
	auto enemy_market = w.spawn_at(1, ge("market", "Market"), spot.first - 10.0, spot.second - 2.0);
	// on the other bank of the river: the cart cannot get there
	auto tc_enemy = w.town_center(1);
	const auto other_bank = w.land_near(w.position(tc_enemy).ne.to_double() - 8.0, w.position(tc_enemy).se.to_double() + 8.0, 10);
	auto across = other_bank ? w.spawn_at(0, ge("market", "Market"), other_bank->first, other_bank->second) : nullptr;
	auto cart = w.unit_near(0, "trade_cart", "TradeCart", spot.first + 3.5, spot.second);
	auto militia = w.unit_near(0, "militia", "Militia", spot.first + 3.5, spot.second + 4.0);
	if (home == nullptr or far == nullptr or enemy_market == nullptr or across == nullptr or cart == nullptr or militia == nullptr) {
		check(false, "markets and cart for the check");
		return;
	}
	w.advance(0.3);
	const auto *type = w.special()->type_of(w.state, cart, w.now);
	check(type and type->trader, "the cart is a trade unit");
	check(type and std::abs(type->work_rate - trade::TRADE_WORK_RATE) < 1e-6, "work rate of the data is the one of the rules");
	check(not w.special()->order_trade(w.state, militia->get_id(), far->get_id(), w.now).empty(), "a militia does not trade");
	check(not w.special()->order_trade(w.state, cart->get_id(), enemy_market->get_id(), w.now).empty(), "no trade with an enemy market");
	check(not w.special()->order_trade(w.state, cart->get_id(), cart->get_id(), w.now).empty(), "a cart is no market");

	// by right click (through the command handling)
	w.command(command_t::MOVE, {cart}, far->get_id(), w.position(far));
	w.advance(0.3);
	const auto job = w.special()->get_info(cart->get_id());
	check(job and job->job.find("Handelt") == 0, "the HUD shows the trade: " + (job ? job->job : std::string{}));
	check(w.special()->get_job_target(cart->get_id()).has_value(), "the right click on the market starts the route");

	// expected gold of one trip with the sizes of the data
	auto hs = w.state->get_combat()->get_stats(home->get_id());
	auto fs = w.state->get_combat()->get_stats(far->get_id());
	const auto ph = w.position(home);
	const auto pf = w.position(far);
	const double dx = std::max(0.0, std::abs(ph.ne.to_double() - pf.ne.to_double()) - 2.0 * fs->radius - 2.0 * hs->radius);
	const double dy = std::max(0.0, std::abs(ph.se.to_double() - pf.se.to_double()) - 2.0 * fs->radius - 2.0 * hs->radius);
	const double width = static_cast<double>(w.state->get_map()->get_size()[0]);
	const double expected = trade::trade_gold(std::max(0.1, std::sqrt(dx * dx + dy * dy)), trade::TRADE_WORK_RATE, trade::TRADE_SPEED_CART, width);

	w.state->get_player(0)->get_resources().set({0.0, 0.0, 0.0, 0.0});
	const auto before = w.special()->counters();
	check(w.advance_until(60.0, [&]() { return w.special()->counters().trade_trips >= before.trade_trips + 1; }),
	      "the cart arrives and is paid");
	const double gold1 = w.resource(0, resource_t::GOLD);
	check(std::abs(gold1 - expected) < 0.01, "gold of the trip: " + std::to_string(gold1) + " expected " + std::to_string(expected));
	w.advance(1.0);
	check(w.resource(0, resource_t::GOLD) == gold1, "the arrival pays only once");

	// the way back pays nothing; the second arrival pays again
	const auto mid = w.now.to_double();
	check(w.advance_until(80.0, [&]() { return w.special()->counters().trade_trips >= before.trade_trips + 2; }),
	      "a second trip after the return");
	const double round_trip = w.now.to_double() - mid;
	check(std::abs(w.resource(0, resource_t::GOLD) - 2.0 * expected) < 0.01, "two trips pay twice, the return pays nothing");

	// the caravan shortens the round trip, the gold of a trip stays
	w.production->grant_research(0, "Caravan");
	w.advance(0.3);
	check(w.advance_until(60.0, [&]() { return w.special()->counters().trade_trips >= before.trade_trips + 3; }), "third trip");
	const double t3 = w.now.to_double();
	const double gold3 = w.resource(0, resource_t::GOLD);
	check(w.advance_until(60.0, [&]() { return w.special()->counters().trade_trips >= before.trade_trips + 4; }), "fourth trip");
	const double fast_trip = w.now.to_double() - t3;
	check(fast_trip < round_trip * 0.8, "caravan: round trip " + std::to_string(fast_trip) + " s instead of " + std::to_string(round_trip) + " s");
	check(std::abs(w.resource(0, resource_t::GOLD) - gold3 - expected) < 0.01, "caravan: same gold per trip");

	// the route comes back with the save game
	const auto text = save::serialize(save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "W7"));
	check(text.find("trade = ") != std::string::npos, "the save game has the route");
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parsed " + error);
	World l{opt, quiet_map()};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	check(result.ok, "restored " + result.error);
	l.advance(0.5);
	const auto carts = l.of(0, "TradeCart");
	check(carts.size() == 1 and l.special()->get_job_target(carts.front()->get_id()).has_value(), "the cart goes on after loading");
	if (not carts.empty()) {
		const double loaded_gold = l.resource(0, resource_t::GOLD);
		check(l.advance_until(60.0, [&]() { return l.resource(0, resource_t::GOLD) > loaded_gold; }), "it earns gold after loading");
	}

	// an unreachable market ends the route instead of keeping the cart busy for ever
	check(w.special()->order_trade(w.state, cart->get_id(), across->get_id(), w.now).empty(), "route to the other bank ordered");
	const bool gave_up = w.advance_until(120.0, [&]() { return not w.special()->get_job_target(cart->get_id()).has_value(); });
	check(gave_up, "the route to the other bank ends, cart at (" + std::to_string(w.position(cart).ne.to_double()) + ", "
	                   + std::to_string(w.position(cart).se.to_double()) + ")");

	w.combat()->kill(w.state, across->get_id(), w.now);
	w.advance(0.3);

	// a new order ends the route, a lost market as well
	w.command(command_t::MOVE, {cart}, std::nullopt, w.position(militia));
	w.advance(0.3);
	check(not w.special()->get_job_target(cart->get_id()).has_value(), "walking away ends the route");
	check(w.special()->order_trade(w.state, cart->get_id(), far->get_id(), w.now).empty(), "ordered again");
	w.advance(0.3);
	w.combat()->kill(w.state, home->get_id(), w.now);
	w.advance(1.0);
	check(not w.special()->get_job_target(cart->get_id()).has_value(), "no route without the home market");
	check(not w.special()->order_trade(w.state, cart->get_id(), far->get_id(), w.now).empty(), "one market is not enough");
}

/// S12: a save during a conversion goes on after loading
void check_diplomacy(const Options &opt) {
	std::cout << "== S16 Haltungen" << std::endl;
	using diplomacy::stance_t;
	using component::command::command_t;
	World w{opt, quiet_map()};
	w.advance(0.5);
	auto combat = w.state->get_combat();
	auto c = scene_spot(w, 8.0, -8.0);
	auto own = w.unit_near(0, "militia", "Militia", c.first, c.second);
	auto other = w.unit_near(1, "militia", "Militia", c.first + 1.5, c.second);
	auto villager = w.unit_near(1, "villager", "Villager", c.first, c.second + 1.5);
	auto monk = w.unit_near(0, "monk", "Monk", c.first - 3.0, c.second);
	auto cart = w.unit_near(0, "trade_cart", "TradeCart", c.first - 6.0, c.second - 6.0);
	if (own == nullptr or other == nullptr or villager == nullptr or monk == nullptr or cart == nullptr) {
		check(false, "scene for the stances");
		return;
	}
	check(combat->stance(0, 1) == stance_t::ENEMY and combat->stance(1, 0) == stance_t::ENEMY, "players start as enemies");
	check(combat->stance(0, 0) == stance_t::ALLY, "a player is on his own side");
	const player_id_t gaia = 2;
	combat->set_stance(0, gaia, stance_t::ALLY);
	check(combat->stance(0, gaia) == stance_t::ENEMY and combat->get_stances().empty(), "gaia has no stance");

	// allies on both sides: side by side without a fight
	combat->set_stance(0, 1, stance_t::ALLY);
	combat->set_stance(1, 0, stance_t::ALLY);
	check(not combat->order_attack(w.state, own->get_id(), other->get_id(), w.now), "no ordered attack on an ally");
	const auto before = w.health(other);
	const auto before_own = w.health(own);
	w.advance(8.0);
	check(w.health(other) == before and w.health(own) == before_own, "allies do not fight");
	w.command(command_t::MOVE, {own}, other->get_id(), w.position(other));
	w.advance(0.5);
	check(not combat->get_target(own->get_id()).has_value(), "the right click on an ally is no attack");
	check(not w.special()->order_convert(w.state, monk->get_id(), other->get_id(), w.now).empty(), "a monk does not convert an ally");

	// the ally betrays: one side enemy again, only that side attacks
	combat->set_stance(1, 0, stance_t::ENEMY);
	check(w.advance_until(10.0, [&]() { return w.health(own) < before_own; }), "the former ally attacks");
	check(not combat->order_attack(w.state, own->get_id(), other->get_id(), w.now), "the other side stays allied");

	// a running fight ends with the alliance
	combat->set_stance(0, 1, stance_t::ENEMY);
	check(combat->order_attack(w.state, own->get_id(), other->get_id(), w.now), "enemies fight");
	w.advance(1.0);
	combat->set_stance(0, 1, stance_t::ALLY);
	combat->set_stance(1, 0, stance_t::ALLY);
	w.advance(4.0);
	const auto settled = w.health(other);
	const auto settled_own = w.health(own);
	w.advance(4.0);
	check(w.health(other) == settled and w.health(own) == settled_own, "the alliance ends the fight");

	// neutral: soldiers yes, villagers only by order
	combat->set_stance(0, 1, stance_t::NEUTRAL);
	check(combat->order_attack(w.state, own->get_id(), other->get_id(), w.now), "an ordered attack on a neutral player");
	check(w.advance_until(10.0, [&]() { return w.health(other) < settled; }), "the neutral soldier is hit");
	w.set_health(other, 0);
	w.advance(1.0);
	const auto vill = w.health(villager);
	w.advance(8.0);
	check(w.health(villager) == vill, "no automatic attack on a neutral villager");
	check(combat->order_attack(w.state, own->get_id(), villager->get_id(), w.now), "by order the villager is a target");
	check(w.advance_until(10.0, [&]() { return w.health(villager) < vill; }), "the ordered attack hits the villager");

	// save game
	auto data = save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "S16");
	const auto text = save::serialize(data);
	check(text.find("stance = 1 neutral") != std::string::npos and text.find("stance = 0 ally") != std::string::npos, "stances stored");
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parsed " + error);
	World l{opt, quiet_map()};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	check(result.ok, "restored " + result.error);
	check(l.state->get_combat()->stance(0, 1) == stance_t::NEUTRAL and l.state->get_combat()->stance(1, 0) == stance_t::ALLY, "stances after loading");
	save::SaveData bad;
	std::string bad_text = text;
	bad_text.replace(bad_text.find("stance = 1 neutral"), 18, "stance = 1 friend!");
	check(not save::parse(bad_text, bad, error), "an unknown stance is rejected");

	// trade with the market of an ally
	World t{opt, quiet_map()};
	t.advance(0.5);
	auto tc = t.state->get_combat();
	auto s = scene_spot(t, 8.0, -8.0);
	auto home = t.spawn_at(0, ge("market", "Market"), s.first, s.second);
	const auto far_spot = t.land_near(s.first + 8.0, s.second + 14.0, 8).value_or(std::pair{s.first + 8.0, s.second + 14.0});
	auto partner = t.spawn_at(1, ge("market", "Market"), far_spot.first, far_spot.second);
	auto trader = t.unit_near(0, "trade_cart", "TradeCart", s.first + 3.5, s.second);
	if (home == nullptr or partner == nullptr or trader == nullptr) {
		check(false, "scene for the allied trade");
		return;
	}
	t.advance(0.3);
	check(not t.special()->order_trade(t.state, trader->get_id(), partner->get_id(), t.now).empty(), "no trade with an enemy");
	tc->set_stance(0, 1, stance_t::ALLY);
	tc->set_stance(1, 0, stance_t::ALLY);
	check(t.special()->order_trade(t.state, trader->get_id(), partner->get_id(), t.now).empty(), "trade with an ally");
	const auto gold0 = t.resource(0, resource_t::GOLD);
	const auto gold1 = t.resource(1, resource_t::GOLD);
	check(t.advance_until(60.0, [&]() { return t.resource(0, resource_t::GOLD) > gold0; }), "the trade with the ally brings gold");
	check(t.resource(1, resource_t::GOLD) == gold1, "the gold goes to the owner of the cart");
	tc->set_stance(0, 1, stance_t::ENEMY);
	check(t.advance_until(5.0, [&]() { return not t.special()->get_job_target(trader->get_id()).has_value(); }), "the end of the alliance ends the route");

	// the stance from the game menu: visible at once, the computer answers with the same stance
	t.advance(0.5);
	const auto before_menu = t.production->snapshot();
	check(before_menu.stances.size() == 1 and before_menu.stances[0].first == 1, "the menu lists the other player, not gaia");
	t.production->request_stance(1, static_cast<int>(stance_t::NEUTRAL));
	check(t.production->snapshot().stances[0].second == static_cast<int>(stance_t::NEUTRAL), "the menu shows the new stance at once");
	t.advance(0.3);
	check(tc->stance(0, 1) == stance_t::NEUTRAL and tc->stance(1, 0) == stance_t::NEUTRAL, "the stance holds for both sides");
	check(t.production->snapshot().stances[0].second == static_cast<int>(stance_t::NEUTRAL), "the menu keeps the stance after the update");
}

void check_players(const Options &opt) {
	std::cout << "== S17 Mehr als zwei Spieler" << std::endl;
	using combat::match_state_t;
	using diplomacy::stance_t;
	MapSettings base = quiet_map();
	base.players = 4;
	base.teams = {1, 1, 2, 2};
	World w{opt, base};
	w.advance(0.5);
	auto combat = w.state->get_combat();
	for (player_id_t p = 0; p < 4; ++p) {
		check(w.town_center(p) != nullptr, "town center of player " + std::to_string(p));
		check(w.of(p, "Villager").size() == 3, "three villagers of player " + std::to_string(p));
	}
	check(combat->stance(0, 1) == stance_t::ALLY and combat->stance(1, 0) == stance_t::ALLY
	          and combat->stance(2, 3) == stance_t::ALLY and combat->stance(0, 2) == stance_t::ENEMY
	          and combat->stance(3, 1) == stance_t::ENEMY,
	      "teams start as allies");

	// save game with the map settings of the players; a changed stance replaces the team
	combat->set_stance(2, 3, stance_t::ENEMY);
	auto data = save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "S17");
	const auto text = save::serialize(data);
	check(text.find("players = 4") != std::string::npos and text.find("teams = 1 1 2 2") != std::string::npos, "players and teams stored");
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parsed " + error);
	check(parsed.map.players == 4 and parsed.map.teams == base.teams, "players and teams read");
	World l{opt, parsed.map};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	check(result.ok, "restored " + result.error);
	check(l.state->get_combat()->stance(2, 3) == stance_t::ENEMY and l.state->get_combat()->stance(3, 2) == stance_t::ALLY
	          and l.state->get_combat()->stance(0, 1) == stance_t::ALLY,
	      "stances of the file after loading");
	combat->set_stance(2, 3, stance_t::ALLY);

	// the last team wins together: everything of players 2 and 3 goes
	w.advance(1.0);
	check(combat->get_match_state(0) == match_state_t::RUNNING, "the game runs");
	const auto kill = [&](player_id_t p) {
		std::vector<entity_id_t> ids;
		for (const auto &[id, entity] : w.state->get_game_entities()) {
			if (w.owner(entity) == p and combat->get_stats(id) != nullptr and not combat->is_dead(id)) {
				ids.push_back(id);
			}
		}
		for (auto id : ids) {
			combat->kill(w.state, id, w.now);
		}
	};
	kill(2);
	w.advance(4.0);
	check(combat->get_match_state(2) == match_state_t::DEFEAT and combat->get_match_state(0) == match_state_t::RUNNING,
	      "one player of the other team is out, the game goes on");
	kill(3);
	check(w.advance_until(10.0, [&]() { return combat->get_match_state(0) != match_state_t::RUNNING; }), "the match ends");
	check(combat->get_match_state(0) == match_state_t::VICTORY and combat->get_match_state(1) == match_state_t::VICTORY
	          and combat->get_match_state(3) == match_state_t::DEFEAT,
	      "the remaining team wins together");

	// three computer players on one side: allied computers leave each other alone
	MapSettings all = base;
	all.ai.mode = ai_mode_t::ON;
	all.ai.first_attack = 30.0;
	all.ai.attack_size = 1;
	all.teams = {1, 1, 1, 1};
	World a{opt, all};
	check(a.game->get_ai_players().size() == 3, "one computer per player but the first");
	a.advance(0.5);
	const auto units_start = a.of(0, "Villager").size();
	const auto tc0 = a.town_center(0);
	const auto tc0_health = a.health(tc0);
	a.advance(240.0);
	check(a.health(tc0) == tc0_health, "allied computers do not attack the town center of player 0");
	check(a.of(0, "Villager").size() == units_start, "allied computers do not kill the villagers of player 0");
	check(a.state->get_combat()->get_match_state(0) == match_state_t::RUNNING, "allies without anyone out go on playing");
	for (player_id_t p = 1; p < 4; ++p) {
		check(a.of(p, "Villager").size() > units_start, "computer " + std::to_string(p) + " trains villagers");
	}

	// enemies of two computers are found by both
	MapSettings war = base;
	war.ai.mode = ai_mode_t::ON;
	war.ai.first_attack = 30.0;
	war.ai.attack_size = 1;
	war.teams = {};
	World e{opt, war};
	e.advance(240.0);
	check(e.state->get_combat()->stance(1, 2) == stance_t::ENEMY, "without teams everyone is an enemy");
	std::cout << "  ok   computers after 240 s: " << e.of(1, "Villager").size() << ", " << e.of(2, "Villager").size() << ", "
	          << e.of(3, "Villager").size() << " villagers" << std::endl;
}

void check_save_converting(const Options &opt) {
	std::cout << "== S12 Spielstand während einer Konvertierung" << std::endl;
	World w{opt, quiet_map()};
	w.advance(0.5);
	auto c = scene_spot(w, 8.0, 8.0);
	auto monk = w.unit_near(0, "monk", "Monk", c.first, c.second);
	auto enemy = w.unit_near(1, "militia", "Militia", c.first + 3.0, c.second);
	if (monk == nullptr or enemy == nullptr) {
		check(false, "scene for the save during a conversion");
		return;
	}
	w.passive(monk);
	w.passive(enemy);
	w.advance(0.3);
	check(w.special()->order_convert(w.state, monk->get_id(), enemy->get_id(), w.now).empty(), "conversion ordered");
	w.advance(0.5);
	const auto monk_id = monk->get_id();
	const auto enemy_id = enemy->get_id();
	check(w.special()->get_job_target(monk_id) == enemy_id and w.owner(enemy) == 1, "conversion runs at the save");
	auto data = save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "S12");
	const auto text = save::serialize(data);
	check(text.find("convert_target = ") != std::string::npos, "convert_target stored");
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parsed " + error);
	World l{opt, quiet_map()};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	check(result.ok, "restored " + result.error);
	l.advance(0.5);
	auto monks = l.of(0, "Monk");
	if (monks.empty()) {
		check(false, "monk after loading");
		return;
	}
	auto target = l.special()->get_job_target(monks.front()->get_id());
	check(target.has_value(), "the monk goes on converting after loading");
	if (target) {
		auto militia = l.state->get_game_entity(*target);
		check(l.advance_until(60.0, [&]() { return not l.alive(militia) or l.owner(militia) == 0; }),
		      "the conversion ends after loading");
	}
}

void check_save(const Options &opt) {
	std::cout << "== S9 Spielstand Version 6" << std::endl;
	World w{opt, quiet_map()};
	w.advance(0.5);
	auto relics = w.named("Relic");
	if (relics.size() < 2) {
		check(false, "relics for the save");
		return;
	}
	auto rp = w.position(relics[0]);
	auto monk = w.unit_near(0, "monk", "Monk", rp.ne.to_double() + 1.5, rp.se.to_double());
	auto mspot = w.land_near(rp.ne.to_double() + 5.0, rp.se.to_double() + 3.0, 12);
	auto treb_spot = scene_spot(w, -6.0, -8.0);
	if (monk == nullptr or not mspot) {
		check(false, "scene for the save");
		return;
	}
	auto monastery = w.spawn_at(0, ge("monastery", "Monastery"), mspot->first, mspot->second);
	auto treb = w.spawn_at(0, ge("trebuchet", "Trebuchet"), treb_spot.first, treb_spot.second);
	w.passive(monk);
	w.advance(0.5);
	w.special()->order_pickup(w.state, monk->get_id(), relics[0]->get_id(), w.now);
	w.advance_until(30.0, [&]() { return w.special()->carried_relic(monk->get_id()).has_value(); });
	w.special()->order_deliver(w.state, monk->get_id(), monastery->get_id(), w.now);
	w.advance_until(40.0, [&]() { return not w.special()->stowed(monastery->get_id()).empty(); });
	auto rp2 = w.position(relics[1]);
	auto monk2 = w.unit_near(0, "monk", "Monk", rp2.ne.to_double() + 1.5, rp2.se.to_double());
	if (monk2) {
		w.passive(monk2);
		w.advance(0.3);
		w.special()->order_pickup(w.state, monk2->get_id(), relics[1]->get_id(), w.now);
		w.advance_until(30.0, [&]() { return w.special()->carried_relic(monk2->get_id()).has_value(); });
	}
	w.special()->set_packed(w.state, treb->get_id(), false, w.now);
	w.advance(special::SpecialState::PACK_TIME + 0.5);
	// a monk with faith below 100: convert an enemy first
	auto enemy = w.unit_near(1, "militia", "Militia", rp.ne.to_double() + 4.0, rp.se.to_double() - 2.0);
	auto monk3 = w.unit_near(0, "monk", "Monk", rp.ne.to_double() + 1.0, rp.se.to_double() - 2.0);
	if (enemy and monk3) {
		w.passive(enemy);
		w.passive(monk3);
		w.advance(0.3);
		w.special()->order_convert(w.state, monk3->get_id(), enemy->get_id(), w.now);
		w.advance_until(20.0, [&]() { return w.owner(enemy) == 0; });
		w.advance(3.0);
	}
	const double faith_before = monk3 ? w.special()->faith(monk3->get_id()).value_or(-1.0) : -1.0;
	check(faith_before > 0.0 and faith_before < 100.0, "a monk with faith " + fmt(faith_before));

	auto data = save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "W6");
	const auto text = save::serialize(data);
	check(text.find("version = 9") != std::string::npos and text.find("relics = 1") != std::string::npos,
	      "save version 6 with relics on the map");
	for (const auto &key : {"faith = ", "relic_carry = ", "relic_store = ", "siege_mode = unpacked"}) {
		check(text.find(key) != std::string::npos, std::string{"key '"} + key + "' stored");
	}
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parsed " + error);
	World l{opt, quiet_map()};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	check(result.ok, "restored " + result.error);
	l.advance(0.5);
	auto monasteries = l.of(0, "Monastery");
	check(monasteries.size() == 1 and l.special()->stowed(monasteries.front()->get_id()).size() == 1,
	      "relic back in the monastery");
	size_t carrying = 0;
	double faith_after = -1.0;
	for (const auto &m : l.of(0, "Monk")) {
		if (l.special()->carried_relic(m->get_id())) {
			carrying += 1;
			check(m->get_shown_animation_path().find("carry_override_monk") != std::string::npos,
			      "carry graphics after loading: " + m->get_shown_animation_path());
		}
		auto f = l.special()->faith(m->get_id());
		if (f and *f < 99.0) {
			faith_after = *f;
		}
	}
	check(carrying == (monk2 ? 1u : 0u), "carried relic restored");
	check(std::abs(faith_after - faith_before) < 1.0, "faith restored: " + fmt(faith_after));
	auto trebs = l.of(0, "Trebuchet");
	check(trebs.size() == 1 and l.special()->is_unpacked(trebs.front()->get_id()), "trebuchet unpacked after loading");
	if (trebs.size() == 1) {
		check(trebs.front()->get_shown_animation_path().find("idle_form_override_trebuchet") != std::string::npos,
		      "unpacked graphic after loading: " + trebs.front()->get_shown_animation_path());
	}
	auto again = save::capture(l.state, l.settings, l.game->get_generated_entity_range(), l.now, "W6");
	auto count = [](const save::SaveData &d) {
		size_t n = 0;
		for (const auto &e : d.entities) {
			n += (e.faith ? 1 : 0) + (e.relic_carry ? 1 : 0) + e.relic_store.size() + (e.siege_mode.empty() ? 0 : 1);
		}
		return n;
	};
	check(count(again) == count(data), "W6 keys after save -> load -> save (" + std::to_string(count(again)) + ")");
	// version 5 file: no relics on its map
	std::string v5 = text;
	v5.replace(v5.find("version = 9"), 11, "version = 5");
	v5.erase(v5.find("relics = 1\n"), 11);
	save::SaveData old;
	check(save::parse(v5, old, error) and not old.map.relics, "version 5 file parses, map without relics");
}

// ------------------------------------------------------------------ S10

void check_ai(const Options &opt) {
	std::cout << "== S10 Computer-Gegner: Kloster, Mönche, Belagerung" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::ON;
	base.ai.player = 1;
	base.ai.first_attack = 100000.0;
	World w{opt, base};
	w.advance(1.0);
	w.production->grant_resources(1, 8000.0);
	w.production->grant_research(1, "FeudalAge");
	w.advance(0.5);
	w.production->grant_research(1, "CastleAge");
	w.advance(0.5);
	double t_monastery = -1.0;
	double t_monk = -1.0;
	double t_workshop = -1.0;
	double t_siege = -1.0;
	while (w.now.to_double() < opt.ai_limit and (t_monk < 0.0 or t_siege < 0.0)) {
		w.advance(5.0);
		for (const auto &m : w.of(1, "Monastery")) {
			if (t_monastery < 0.0 and prod::is_finished(m)) {
				t_monastery = w.now.to_double();
			}
		}
		for (const auto &m : w.of(1, "SiegeWorkshop")) {
			if (t_workshop < 0.0 and prod::is_finished(m)) {
				t_workshop = w.now.to_double();
			}
		}
		if (t_monk < 0.0 and not w.of(1, "Monk").empty()) {
			t_monk = w.now.to_double();
		}
		if (t_siege < 0.0 and (not w.of(1, "Ram").empty() or not w.of(1, "Mangonel").empty())) {
			t_siege = w.now.to_double();
		}
	}
	std::cout << "  AI: monastery t=" << t_monastery << " s, monk t=" << t_monk << " s, workshop t=" << t_workshop
	          << " s, siege t=" << t_siege << " s" << std::endl;
	check(t_monastery > 0.0, "AI built a monastery");
	check(t_monk > 0.0, "AI trained a monk");
	check(t_workshop > 0.0, "AI built a siege workshop");
	check(t_siege > 0.0, "AI trained siege");
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
		else if (a == "--coast-seed") {
			opt.coast_seed = static_cast<uint32_t>(std::stoul(next()));
		}
		else if (a == "--size") {
			opt.size = std::stoul(next());
		}
		else if (a == "--ai-limit") {
			opt.ai_limit = std::stod(next());
		}
		else if (a == "--only") {
			only = next();
		}
		else {
			std::cerr << "usage: openage-special-game-check --root <dir> [--modpack id] [--seed n] [--coast-seed n]"
			             " [--size n] [--ai-limit s] [--only map|monks|relics|siege|ships|save|gaps|ai]"
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
		if (only.empty() or only == "map") {
			check_map(opt);
		}
		if (only.empty() or only == "market") {
			check_market(opt);
		}
		if (only.empty() or only == "trade") {
			check_trade(opt);
		}
		if (only.empty() or only == "diplomacy") {
			check_diplomacy(opt);
		}
		if (only.empty() or only == "players") {
			check_players(opt);
		}
		if (only.empty() or only == "monks") {
			check_monks(opt);
		}
		if (only.empty() or only == "relics") {
			check_relics(opt);
		}
		if (only.empty() or only == "siege") {
			check_siege(opt);
		}
		if (only.empty() or only == "ships") {
			check_ships(opt);
		}
		if (only.empty() or only == "save") {
			check_save(opt);
			check_save_converting(opt);
		}
		if (only.empty() or only == "gaps") {
			check_gaps(opt);
		}
		if (only.empty() or only == "gaps" or only == "trebuchet") {
			check_trebuchet_fire(opt);
		}
		if (only.empty() or only == "ai") {
			check_ai(opt);
		}
	}
	catch (std::exception &err) {
		std::cout << "special game check FAILED: " << err.what() << std::endl;
		return 1;
	}
	if (failures == 0) {
		std::cout << "special game check ok (" << passed << " checks)" << std::endl;
		return 0;
	}
	std::cout << "special game check: " << failures << " failures, " << passed << " ok" << std::endl;
	return 1;
}
