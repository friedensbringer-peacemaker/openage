// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Game state check of the special abilities (XR fork W6), headless, with the converted
 * modpack but without renderer:
 *
 *   openage-special-game-check --root <dir> [--modpack hd_base] [--seed N] [--coast-seed N] [--size N]
 *                              [--ai-limit S] [--only map|monks|relics|siege|ships|save|ai]
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
	w.production->grant_research(1, "Heresy");
	w.advance(0.5);
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
	reason = w.special()->order_deliver(w.state, monk->get_id(), monastery->get_id(), w.now);
	check(reason.empty(), "monk sent to the monastery" + (reason.empty() ? std::string{} : ": " + reason));
	check(w.advance_until(40.0, [&]() { return w.special()->stowed(monastery->get_id()).size() == 1; }),
	      "relic in the monastery");
	check(not w.special()->carried_relic(monk->get_id()), "monk hands the relic over");
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
	auto ram = w.unit_near(0, "ram", "Ram", c2.first + 3.5, c2.second + 0.5);
	if (house == nullptr or guard == nullptr or ram == nullptr) {
		check(false, "scene for the ram");
		return;
	}
	w.passive(guard);
	w.advance(0.3);
	const auto guard_h0 = w.health(guard);
	w.command(component::command::command_t::ATTACK, {ram}, guard->get_id(), w.position(guard));
	w.advance(10.0);
	check(w.health(guard) == guard_h0, "ram does not attack a unit (ordered or on its own)");
	const auto house_h0 = w.health(house);
	w.combat()->order_attack(w.state, ram->get_id(), house->get_id(), w.now);
	check(w.advance_until(30.0, [&]() { return w.health(house) < house_h0; }), "ram damages the house");

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
	auto r = w.special()->set_packed(w.state, treb->get_id(), false, w.now);
	check(r.empty(), "trebuchet unpacks" + (r.empty() ? std::string{} : ": " + r));
	check(w.advance_until(special::SpecialState::PACK_TIME + 1.0, [&]() { return w.special()->is_unpacked(treb->get_id()); }),
	      "unpacked after " + fmt(special::SpecialState::PACK_TIME) + " s");
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
	}
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
	check(text.find("version = 6") != std::string::npos and text.find("relics = 1") != std::string::npos,
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
		carrying += l.special()->carried_relic(m->get_id()) ? 1 : 0;
		auto f = l.special()->faith(m->get_id());
		if (f and *f < 99.0) {
			faith_after = *f;
		}
	}
	check(carrying == (monk2 ? 1u : 0u), "carried relic restored");
	check(std::abs(faith_after - faith_before) < 1.0, "faith restored: " + fmt(faith_after));
	auto trebs = l.of(0, "Trebuchet");
	check(trebs.size() == 1 and l.special()->is_unpacked(trebs.front()->get_id()), "trebuchet unpacked after loading");
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
	v5.replace(v5.find("version = 6"), 11, "version = 5");
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
			             " [--size n] [--ai-limit s] [--only map|monks|relics|siege|ships|save|ai]"
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
