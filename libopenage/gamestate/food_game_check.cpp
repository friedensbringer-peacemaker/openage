// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Game state check of the food economy (XR fork W3), headless, with the converted
 * modpack but without renderer:
 *
 *   openage-food-game-check --root <dir> [--modpack hd_base] [--seed N] [--size N] [--ai-limit S]
 *                           [--coast-seed N]
 *
 * Random maps (64 tiles), simulated time advances in steps of 50 ms through the event
 * loop with the production of the simulation (like GameSimulation::run).
 *
 *  F1. Map: sheep in sight of every town centre (they belong to its player after the
 *      first food tick), sheep further away stay gaia, a boar and deer per start.
 *  F2. Herdables: a sheep walks where it is sent (herding); a gaia sheep goes to the
 *      player whose unit sees it; an owned one goes to a foreign unit next to it once
 *      no unit of its owner sees it.
 *  F3. Slaughter: a villager kills its sheep (carcass, 7 HP), gathers it, drops the
 *      food at the town centre; the carcass loses food while it is gathered (decay of
 *      the data), it is removed when it is empty.
 *  F4. Hunting: a villager hunts a deer (it flees when hit, carcass), carries more
 *      than 10 (hunter capacity of the data); soldiers attack animals on a right click.
 *  F5. Boar: it fights back and chases the hunter; several villagers kill it.
 *  F6. Farms: not offered before a mill, offered after it; a placed farm does not
 *      block its tiles, shows the farm terrain, gets the food of the data when done,
 *      its builder farms it, the food sinks while it is farmed; one farmer per farm;
 *      placement preview of the farm is an outline only.
 *  F7. Reseeding: the mill offers "Feld neu einsäen", a reseed pays 60 wood at once,
 *      shows as a symbol in the mill queue, a cancel refunds it fully; an empty farm
 *      uses a queued reseed (the farmer builds it again and farms on), without one it
 *      decays, a right click builds it again (costs), after the decay time it is gone.
 *  F8. Fishing (coast map): a dock only next to the shore (not on land), fishing ship
 *      trained at the dock, it fishes and drops the food at the dock; a fishing trap on
 *      water, built by the ship and fished.
 *  F9. Save game version 4: converted sheep, a carcass, a farm, queued reseeds; save ->
 *      load -> save gives the same state; a version 3 file still loads (map without animals).
 *  F10. Computer opponent: builds a mill and farms, herds and hunts, reaches the Feudal
 *      Age (time printed; earlier than 18:40 of 0.6.0-xr.0.14).
 *
 * Prints "food game check ok" and exits with 0 if everything passes.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/internal/activity.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/gather.h"
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
#include "gamestate/catalog.h"
#include "gamestate/econ.h"
#include "gamestate/food.h"
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

struct Options {
	util::Path root;
	std::string modpack = "hd_base";
	uint32_t seed = 1;
	uint32_t coast_seed = 3;
	size_t size = 64;
	double ai_limit = 1500.0;
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

	/// advance until done() or the limit (seconds), true if done
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

	std::shared_ptr<food::FoodState> food() const {
		return this->state->get_food();
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

	double distance(const std::shared_ptr<GameEntity> &a, double ne, double se) const {
		auto p = this->position(a);
		return std::hypot(p.ne.to_double() - ne, p.se.to_double() - se);
	}

	double distance(const std::shared_ptr<GameEntity> &a, const std::shared_ptr<GameEntity> &b) const {
		auto p = this->position(b);
		return this->distance(a, p.ne.to_double(), p.se.to_double());
	}

	bool alive(const std::shared_ptr<GameEntity> &e) const {
		return e != nullptr and this->state->get_game_entities().contains(e->get_id())
		       and not this->state->get_combat()->is_dead(e->get_id());
	}

	bool exists(const std::shared_ptr<GameEntity> &e) const {
		return e != nullptr and this->state->get_game_entities().contains(e->get_id());
	}

	/// entities with this nyan short name (any owner, alive or carcass), sorted by id
	std::vector<std::shared_ptr<GameEntity>> named(const std::string &name, bool only_alive = true) const {
		std::vector<std::shared_ptr<GameEntity>> out;
		for (const auto &[id, e] : this->state->get_game_entities()) {
			if (prod::entity_name(e) == name and (not only_alive or not this->state->get_combat()->is_dead(id))) {
				out.push_back(e);
			}
		}
		std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a->get_id() < b->get_id(); });
		return out;
	}

	/// entities of a player with this nyan short name, sorted by id
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

	std::shared_ptr<component::Harvestable> harvestable(const std::shared_ptr<GameEntity> &e) const {
		if (e == nullptr or not e->has_component(component::component_t::HARVESTABLE)) {
			return nullptr;
		}
		return std::dynamic_pointer_cast<component::Harvestable>(e->get_component(component::component_t::HARVESTABLE));
	}

	std::shared_ptr<component::Gather> gather(const std::shared_ptr<GameEntity> &e) const {
		return std::dynamic_pointer_cast<component::Gather>(e->get_component(component::component_t::GATHER));
	}

	/// a finished entity of a player at a tile position
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

	/// a free land tile (passable for land units) near a point, nothing if none within 8 tiles
	std::optional<std::pair<double, double>> free_land_near(double ne, double se) const {
		auto map = this->state->get_map();
		auto grid = map->get_grid_id("hd_base.data.util.path_type.types.Land");
		for (int r = 0; r <= 8; ++r) {
			for (int dy = -r; dy <= r; ++dy) {
				for (int dx = -r; dx <= r; ++dx) {
					if (std::max(std::abs(dx), std::abs(dy)) != r) {
						continue;
					}
					coord::tile t{static_cast<coord::tile_t>(std::floor(ne)) + dx, static_cast<coord::tile_t>(std::floor(se)) + dy};
					if (map->is_passable(grid, t)) {
						return std::pair{static_cast<double>(t.ne) + 0.5, static_cast<double>(t.se) + 0.5};
					}
				}
			}
		}
		return std::nullopt;
	}

	void order(const std::shared_ptr<GameEntity> &unit, const std::shared_ptr<component::command::Command> &command) {
		auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
			unit->get_component(component::component_t::COMMANDQUEUE));
		queue->add_command(this->now, command);
	}

	void gather_from(const std::shared_ptr<GameEntity> &unit, const std::shared_ptr<GameEntity> &target) {
		this->order(unit, std::make_shared<component::command::GatherCommand>(target->get_id()));
	}

	void move_to(const std::shared_ptr<GameEntity> &unit, double ne, double se) {
		auto dest = this->state->get_map()->on_terrain(coord::phys3{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}});
		this->order(unit, std::make_shared<component::command::MoveCommand>(dest));
	}

	const prod::Option *option(const prod::Snapshot &s, const std::string &id) const {
		for (const auto &o : s.options) {
			if (o.id == id) {
				return &o;
			}
		}
		return nullptr;
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

	/**
	 * Place a building of a player with these builders near a point (tries spots in rings until
	 * a new building of the name exists).
	 *
	 * @return The new foundation, nullptr if no spot worked.
	 */
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

	std::string terrain_at(double ne, double se) const {
		return this->state->get_map()->terrain_name(
			coord::tile{static_cast<coord::tile_t>(std::floor(ne)), static_cast<coord::tile_t>(std::floor(se))});
	}
};

bool ends_with(const std::string &s, const std::string &suffix) {
	return s.size() >= suffix.size() and s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ------------------------------------------------------------------ F1 – F5

void check_animals(const Options &opt) {
	std::cout << "== F1 Karte: Schafe, Wildschwein, Hirsche" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	World w{opt, base};
	w.advance(1.0);
	for (player_id_t p = 0; p < 2; ++p) {
		auto tc = w.town_center(p);
		check(tc != nullptr, "town centre of player " + std::to_string(p));
		if (tc == nullptr) {
			return;
		}
		size_t near = 0;
		size_t near_own = 0;
		for (const auto &sheep : w.named("Sheep")) {
			if (w.distance(sheep, tc) <= 8.0) {
				near += 1;
				near_own += w.owner(sheep) == p ? 1 : 0;
			}
		}
		check(near >= 3 and near_own == near, "player " + std::to_string(p) + ": " + std::to_string(near)
		                                          + " sheep in sight of the town centre, all of the player");
	}
	const auto gaia = static_cast<player_id_t>(2);
	size_t far_gaia = 0;
	for (const auto &sheep : w.named("Sheep")) {
		far_gaia += w.owner(sheep) == gaia ? 1 : 0;
	}
	check(far_gaia >= 2, "sheep further away belong to gaia (" + std::to_string(far_gaia) + ")");
	check(w.named("Boar").size() >= 2, "a boar per start (" + std::to_string(w.named("Boar").size()) + ")");
	check(w.named("Deer").size() >= 4, "deer per start (" + std::to_string(w.named("Deer").size()) + ")");
	for (const auto &name : {"Sheep", "Boar", "Deer"}) {
		auto list = w.named(name);
		auto h = list.empty() ? nullptr : w.harvestable(list.front());
		check(h != nullptr and h->get_amount() > 0.0 and h->decay_rate > 0.0 and food::is_animal(h->kind),
		      std::string{name} + ": food " + (h ? std::to_string(h->get_amount()) : "-") + ", decay "
		          + (h ? std::to_string(h->decay_rate) : "-") + "/s (data)");
	}

	std::cout << "== F2 Hüten und Umfärben" << std::endl;
	auto tc0 = w.town_center(0);
	auto tc0p = w.position(tc0);
	std::shared_ptr<GameEntity> own_sheep;
	for (const auto &sheep : w.of(0, "Sheep")) {
		own_sheep = sheep;
	}
	check(own_sheep != nullptr, "own sheep");
	if (own_sheep == nullptr) {
		return;
	}
	auto dest = w.free_land_near(tc0p.ne.to_double() + 3.5, tc0p.se.to_double() + 3.5);
	w.move_to(own_sheep, dest->first, dest->second);
	bool herded = w.advance_until(20.0, [&]() { return w.distance(own_sheep, dest->first, dest->second) < 1.0; });
	check(herded, "sheep walks where it is sent (herding)");

	// a gaia sheep: a villager of player 0 comes into sight
	std::shared_ptr<GameEntity> wild;
	for (const auto &sheep : w.of(gaia, "Sheep")) {
		if (wild == nullptr or w.distance(sheep, tc0) < w.distance(wild, tc0)) {
			wild = sheep;
		}
	}
	check(wild != nullptr, "a gaia sheep");
	if (wild != nullptr) {
		auto wp = w.position(wild);
		auto spot = w.free_land_near(wp.ne.to_double() + 1.5, wp.se.to_double());
		auto scout = w.spawn_at(0, ge("villager", "Villager"), spot->first, spot->second);
		w.advance(1.5);
		check(w.owner(wild) == 0, "gaia sheep goes to the player whose villager sees it");
		// the villager leaves, an enemy villager stands next to the sheep
		auto away = w.free_land_near(tc0p.ne.to_double() + 5.0, tc0p.se.to_double() - 5.0);
		auto spot1 = w.free_land_near(wp.ne.to_double() - 1.0, wp.se.to_double());
		w.move_to(scout, away->first, away->second);
		auto enemy = w.spawn_at(1, ge("villager", "Villager"), spot1->first, spot1->second);
		const bool converted = w.advance_until(40.0, [&]() { return w.owner(wild) == 1; });
		check(converted, "owned sheep goes to the enemy villager next to it once no own unit sees it");
		check(w.food()->counters().conversions >= 4, "conversions counted (" + std::to_string(w.food()->counters().conversions) + ")");
		(void)enemy;
	}

	std::cout << "== F3 Schlachten, Sammeln, Kadaver-Verderb" << std::endl;
	auto villagers = w.of(0, "Villager");
	check(not villagers.empty(), "villagers of player 0");
	auto vil = villagers.front();
	auto sh = w.harvestable(own_sheep);
	const double sheep_food = sh->get_amount();
	const double food0 = w.resource(0, resource_t::FOOD);
	w.gather_from(vil, own_sheep);
	const bool killed = w.advance_until(30.0, [&]() { return sh->dead; });
	check(killed, "villager kills the sheep (carcass)");
	check(w.exists(own_sheep) and w.state->get_combat()->is_dead(own_sheep->get_id()), "carcass stays in the game");
	const double t_dead = w.now.to_double();
	const bool dropped = w.advance_until(60.0, [&]() { return w.resource(0, resource_t::FOOD) > food0 + 9.0; });
	check(dropped, "food of the sheep dropped at the town centre (" + std::to_string(w.resource(0, resource_t::FOOD) - food0) + ")");
	const bool gone = w.advance_until(400.0, [&]() { return not w.exists(own_sheep); });
	const double gathered = w.resource(0, resource_t::FOOD) - food0;
	check(gone, "empty carcass removed after " + std::to_string(w.now.to_double() - t_dead) + " s");
	check(gathered < sheep_food - 1.0 and gathered > 0.4 * sheep_food,
	      "decay while gathering: " + std::to_string(gathered) + " of " + std::to_string(sheep_food) + " food arrived");
	check(w.food()->counters().decayed_food > 1.0, "decayed food " + std::to_string(w.food()->counters().decayed_food));

	std::cout << "== F4 Jagd auf Hirsche" << std::endl;
	std::shared_ptr<GameEntity> deer;
	for (const auto &d : w.named("Deer")) {
		if (deer == nullptr or w.distance(d, tc0) < w.distance(deer, tc0)) {
			deer = d;
		}
	}
	check(deer != nullptr, "a deer");
	if (deer != nullptr) {
		auto dp = w.position(deer);
		auto spot = w.free_land_near(dp.ne.to_double() + 3.0, dp.se.to_double() + 1.0);
		auto hunter = w.spawn_at(0, ge("villager", "Villager"), spot->first, spot->second);
		const size_t flights = w.food()->counters().flights;
		const auto start = w.position(deer);
		w.gather_from(hunter, deer);
		auto dh = w.harvestable(deer);
		const bool dead = w.advance_until(60.0, [&]() { return dh->dead; });
		check(dead, "hunter kills the deer");
		check(w.food()->counters().flights > flights, "the deer flees when it is hit ("
		                                                  + std::to_string(w.food()->counters().flights - flights) + ")");
		check(w.distance(deer, start.ne.to_double(), start.se.to_double()) > 0.5, "the deer ran before it died");
		double carried = 0.0;
		w.advance_until(60.0, [&]() {
			carried = std::max(carried, w.gather(hunter)->get_carried());
			return carried > 10.5;
		});
		check(carried > 10.5, "hunter carries more than 10 (hunter capacity of the data): " + std::to_string(carried));
		// a soldier attacks a deer on a right click (gaia animals as targets of players)
		std::shared_ptr<GameEntity> deer2;
		for (const auto &d : w.named("Deer")) {
			if (d != deer and w.alive(d)) {
				deer2 = d;
			}
		}
		if (deer2 != nullptr) {
			auto p2 = w.position(deer2);
			auto s2 = w.free_land_near(p2.ne.to_double() + 2.0, p2.se.to_double());
			auto soldier = w.spawn_at(0, ge("militia", "Militia"), s2->first, s2->second);
			w.advance(0.2);
			check(w.state->get_combat()->order_attack(w.state, soldier->get_id(), deer2->get_id(), w.now),
			      "militia may attack a gaia deer");
			auto h2 = w.harvestable(deer2);
			check(w.advance_until(60.0, [&]() { return h2->dead; }), "militia kills the deer (carcass)");
			check(w.state->get_combat()->order_attack(w.state, soldier->get_id(), own_sheep->get_id(), w.now) == false,
			      "no attack order on a removed sheep");
		}
	}

	std::cout << "== F5 Wildschwein" << std::endl;
	auto boars = w.named("Boar");
	check(not boars.empty(), "a boar");
	if (not boars.empty()) {
		auto boar = boars.front();
		auto bp = w.position(boar);
		std::vector<std::shared_ptr<GameEntity>> hunters;
		for (int i = 0; i < 6; ++i) {
			auto s = w.free_land_near(bp.ne.to_double() + 1.2 + 0.3 * i, bp.se.to_double() + 0.4);
			hunters.push_back(w.spawn_at(0, ge("villager", "Villager"), s->first, s->second));
		}
		w.advance(0.2);
		const int64_t hp_before = w.state->get_combat()->get_health({hunters.front()->get_id()}).front().health;
		w.gather_from(hunters.front(), boar);
		bool fights = w.advance_until(20.0, [&]() {
			auto target = w.state->get_combat()->get_target(boar->get_id());
			return target.has_value();
		});
		check(fights, "the boar fights back");
		w.advance(4.0);
		int64_t lowest = hp_before;
		for (const auto &h : hunters) {
			if (w.alive(h)) {
				lowest = std::min(lowest, w.state->get_combat()->get_health({h->get_id()}).front().health);
			}
			else {
				lowest = 0;
			}
		}
		check(lowest < hp_before, "a hunter is hurt by the boar");
		for (size_t i = 1; i < hunters.size(); ++i) {
			w.gather_from(hunters[i], boar);
		}
		auto bh = w.harvestable(boar);
		check(w.advance_until(90.0, [&]() { return bh->dead; }), "six villagers kill the boar");
		check(bh->get_amount() > 300.0, "boar carcass with " + std::to_string(bh->get_amount()) + " food");
	}
}

// ------------------------------------------------------------------ F6 – F7

void check_farms(const Options &opt) {
	std::cout << "== F6 Farmen" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	World w{opt, base};
	w.advance(0.5);
	w.add_resources(0, 2000.0);
	auto tc = w.town_center(0);
	auto tcp = w.position(tc);
	for (int i = 0; i < 2; ++i) {
		auto spot = w.free_land_near(tcp.ne.to_double() + 3.0 + i, tcp.se.to_double() + 3.0);
		w.spawn_at(0, ge("villager", "Villager"), spot->first, spot->second);
	}
	w.advance(0.2);
	auto villagers = w.of(0, "Villager");
	check(villagers.size() >= 4, "four villagers");
	w.select({villagers.front()});
	auto snap = w.production->snapshot();
	check(w.option(snap, "Farm") == nullptr and w.option(snap, "Mill") != nullptr, "no farm before a mill");
	w.production->place_for(0, {villagers.front()->get_id()}, "Farm",
	                        coord::phys3{coord::phys_t{tcp.ne.to_double() + 5.0}, coord::phys_t{tcp.se.to_double()}, coord::phys_t{0.0}});
	w.advance(0.3);
	check(w.of(0, "Farm").empty(), "farm rejected without a mill");

	auto mill = w.place_near(0, {villagers[0], villagers[1]}, "Mill", tcp.ne.to_double() - 5.0, tcp.se.to_double() + 5.0);
	check(mill != nullptr, "mill placed");
	if (mill == nullptr) {
		return;
	}
	check(w.advance_until(120.0, [&]() { return prod::is_finished(mill); }), "mill built");
	w.select({villagers.front()});
	snap = w.production->snapshot();
	auto farm_option = w.option(snap, "Farm");
	check(farm_option != nullptr and farm_option->available, "farm offered after the mill");
	const double farm_wood = farm_option ? farm_option->cost[1] : 0.0;
	const double wood0 = w.resource(0, resource_t::WOOD);
	auto farm = w.place_near(0, {villagers[2]}, "Farm", tcp.ne.to_double() + 5.0, tcp.se.to_double() - 1.0);
	check(farm != nullptr, "farm placed");
	if (farm == nullptr) {
		return;
	}
	check(std::abs(w.resource(0, resource_t::WOOD) - (wood0 - farm_wood)) < 1e-6,
	      "farm costs " + std::to_string(farm_wood) + " wood at once");
	auto fp = w.position(farm);
	auto land = w.state->get_map()->get_grid_id("hd_base.data.util.path_type.types.Land");
	check(w.state->get_map()->is_passable(land, coord::tile{static_cast<coord::tile_t>(std::floor(fp.ne.to_double())),
	                                                       static_cast<coord::tile_t>(std::floor(fp.se.to_double()))}),
	      "the farm does not block its tiles");
	check(ends_with(w.terrain_at(fp.ne.to_double(), fp.se.to_double()), ".FarmConstruction1"),
	      "foundation shows the construction terrain: " + w.terrain_at(fp.ne.to_double(), fp.se.to_double()));
	auto fh = w.harvestable(farm);
	auto farmer = villagers[2];
	check(w.advance_until(120.0, [&]() { return prod::is_finished(farm); }), "farm built");
	check(ends_with(w.terrain_at(fp.ne.to_double(), fp.se.to_double()), ".FarmCrops"),
	      "finished farm shows crops: " + w.terrain_at(fp.ne.to_double(), fp.se.to_double()));
	check(std::abs(fh->get_amount() - 175.0) < 1.0 or fh->get_amount() > 100.0,
	      "farm food of the data: " + std::to_string(fh->get_amount()));
	const double farm_food = fh->get_amount();
	w.advance(2.0);
	auto job_target = w.gather(farmer)->get_job().target;
	check(job_target and *job_target == farm->get_id(), "the builder farms the farm without an order");
	w.advance(25.0);
	check(fh->get_amount() < farm_food - 3.0, "farm food sinks while it is farmed: " + std::to_string(fh->get_amount()));
	check(fh->worker and *fh->worker == farmer->get_id(), "the farm is held by its farmer");
	// a second villager: the farm is taken
	w.gather_from(villagers[3], farm);
	w.advance(3.0);
	auto second = w.gather(villagers[3])->get_job().target;
	check(not second or *second != farm->get_id(), "one farmer per farm (the second one does not take it)");
	// placement preview: outline only
	w.select({villagers[0]});
	w.production->start_placement("Farm");
	w.production->set_placement_cursor(coord::phys3{coord::phys_t{tcp.ne.to_double() - 6.0}, coord::phys_t{tcp.se.to_double() - 6.0}, coord::phys_t{0.0}});
	w.advance(0.5);
	auto preview = w.production->placement_preview();
	check(preview.active and preview.outline_only, "farm preview: outline only (no picture)");
	w.production->cancel_placement();
	w.production->start_placement("House");
	w.production->set_placement_cursor(coord::phys3{coord::phys_t{fp.ne.to_double()}, coord::phys_t{fp.se.to_double()}, coord::phys_t{0.0}});
	w.advance(0.5);
	preview = w.production->placement_preview();
	check(preview.active and not preview.outline_only and not preview.valid, "no house on a farm: " + preview.reason);
	w.production->cancel_placement();

	std::cout << "== F7 Neusäen, Verfall, neu bauen" << std::endl;
	w.select({mill});
	snap = w.production->snapshot();
	auto reseed = w.option(snap, "ReseedFarm");
	check(reseed != nullptr and reseed->code == prod::CMD_RESEED_FARM and reseed->label == "Feld neu einsäen"
	          and reseed->cost[1] > 0.0,
	      "mill offers \"Feld neu einsäen\" (" + std::to_string(reseed ? reseed->cost[1] : 0.0) + " wood)");
	double wood = w.resource(0, resource_t::WOOD);
	w.production->command(prod::CMD_RESEED_FARM);
	w.advance(0.3);
	check(std::abs(w.resource(0, resource_t::WOOD) - (wood - reseed->cost[1])) < 1e-6, "reseed paid at once");
	check(w.food()->reseeds(0) == 1, "one reseed queued");
	snap = w.production->snapshot();
	size_t symbols = 0;
	for (const auto &item : snap.queue ? snap.queue->items : std::vector<prod::QueueEntry>{}) {
		symbols += item.id == "ReseedFarm" ? 1 : 0;
	}
	check(symbols == 1, "reseed shown as a symbol in the mill queue");
	w.production->cancel_training_at(snap.queue->items.size() - 1);
	w.advance(0.3);
	check(w.food()->reseeds(0) == 0 and std::abs(w.resource(0, resource_t::WOOD) - wood) < 1e-6,
	      "click on the symbol cancels it with a full refund");
	w.production->command(prod::CMD_RESEED_FARM);
	w.advance(0.3);
	check(w.food()->reseeds(0) == 1, "reseed queued again");
	// the farm runs empty soon
	fh->set_amount(3.0);
	const auto counters = w.food()->counters();
	check(w.advance_until(40.0, [&]() { return w.food()->counters().reseeds_used > counters.reseeds_used; }),
	      "empty farm uses the queued reseed");
	check(w.food()->reseeds(0) == 0, "reseed taken from the queue");
	check(not prod::is_finished(farm), "the farm is a foundation again");
	check(w.advance_until(60.0, [&]() { return prod::is_finished(farm); }), "the farmer builds it again");
	w.advance(2.0);
	job_target = w.gather(farmer)->get_job().target;
	check(job_target and *job_target == farm->get_id() and fh->get_amount() > 100.0,
	      "the farmer farms on (food " + std::to_string(fh->get_amount()) + ")");
	// without a reseed: it decays
	fh->set_amount(3.0);
	check(w.advance_until(40.0, [&]() { return w.food()->is_decayed(farm->get_id()); }), "empty farm without reseed decays");
	check(ends_with(w.terrain_at(fp.ne.to_double(), fp.se.to_double()), ".FarmHarvested"),
	      "decayed farm shows the harvested terrain: " + w.terrain_at(fp.ne.to_double(), fp.se.to_double()));
	w.advance(2.0);
	job_target = w.gather(farmer)->get_job().target;
	check(not job_target or *job_target != farm->get_id(), "the farmer leaves the decayed farm");
	wood = w.resource(0, resource_t::WOOD);
	auto reason = w.food()->rebuild(w.state, farm, farmer, w.now);
	check(reason.empty(), "right click builds the decayed farm again" + (reason.empty() ? std::string{} : ": " + reason));
	check(std::abs(w.resource(0, resource_t::WOOD) - (wood - farm_wood)) < 1e-6, "rebuild costs a farm");
	check(w.advance_until(60.0, [&]() { return prod::is_finished(farm); }), "rebuilt farm finished");
	// decayed farms are removed after the decay time
	fh->set_amount(2.0);
	w.advance_until(40.0, [&]() { return w.food()->is_decayed(farm->get_id()); });
	check(w.advance_until(food::FIELD_DECAY_SECONDS + 10.0, [&]() { return not w.exists(farm); }),
	      "decayed farm removed after " + std::to_string(food::FIELD_DECAY_SECONDS) + " s");
	check(not ends_with(w.terrain_at(fp.ne.to_double(), fp.se.to_double()), ".FarmHarvested"),
	      "ground restored: " + w.terrain_at(fp.ne.to_double(), fp.se.to_double()));
}

// ------------------------------------------------------------------ F8

void check_fishing(const Options &opt) {
	std::cout << "== F8 Hafen, Fischerboot, Fischreuse (Küste)" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	base.biome = map_biome_t::COAST;
	World w{opt, base, opt.coast_seed};
	w.advance(0.5);
	w.add_resources(0, 3000.0);
	auto villagers = w.of(0, "Villager");
	auto tc = w.town_center(0);
	auto tcp = w.position(tc);
	// the water tile next to the shore nearest to the town centre
	auto map = w.state->get_map();
	const auto size = map->get_size();
	std::optional<std::pair<double, double>> shore;
	double best = 1e9;
	for (long se = 2; se + 2 < static_cast<long>(size[1]); ++se) {
		for (long ne = 2; ne + 2 < static_cast<long>(size[0]); ++ne) {
			if (not ends_with(w.terrain_at(ne + 0.5, se + 0.5), ".Water")) {
				continue;
			}
			bool beach = false;
			for (long dy = -2; dy <= 2; ++dy) {
				for (long dx = -2; dx <= 2; ++dx) {
					beach = beach or ends_with(w.terrain_at(ne + dx + 0.5, se + dy + 0.5), ".Beach");
				}
			}
			const double d = std::hypot(ne + 0.5 - tcp.ne.to_double(), se + 0.5 - tcp.se.to_double());
			if (beach and d < best) {
				best = d;
				shore = std::pair{ne + 0.5, se + 0.5};
			}
		}
	}
	check(shore.has_value(), "coast map has water next to a beach (" + std::to_string(best) + " tiles from the town centre)");
	if (not shore) {
		return;
	}
	w.select({villagers.front()});
	auto snap = w.production->snapshot();
	check(w.option(snap, "Dock") != nullptr, "villager offers the dock");
	w.production->place_for(0, {villagers.front()->get_id()}, "Dock",
	                        coord::phys3{coord::phys_t{tcp.ne.to_double() + 3.0}, coord::phys_t{tcp.se.to_double() + 3.0}, coord::phys_t{0.0}});
	w.advance(0.3);
	check(w.of(0, "Dock").empty(), "no dock on land");
	auto dock = w.place_near(0, {villagers[0], villagers[1]}, "Dock", shore->first, shore->second, 6);
	check(dock != nullptr, "dock placed at the shore");
	if (dock == nullptr) {
		return;
	}
	auto dp = w.position(dock);
	check(w.terrain_at(dp.ne.to_double(), dp.se.to_double()).find(".Water") != std::string::npos
	          or ends_with(w.terrain_at(dp.ne.to_double(), dp.se.to_double()), "Shallows"),
	      "dock stands on water: " + w.terrain_at(dp.ne.to_double(), dp.se.to_double()));
	check(w.advance_until(180.0, [&]() { return prod::is_finished(dock); }), "dock built");
	w.select({dock});
	snap = w.production->snapshot();
	auto ship_option = w.option(snap, "FishingShip");
	check(ship_option != nullptr and ship_option->available, "dock trains fishing ships");
	check(w.option(snap, "Galley") == nullptr and w.option(snap, "TransportShip") == nullptr, "no warships yet (W6)");
	w.production->train_in(dock->get_id(), "FishingShip");
	check(w.advance_until(ship_option->time + 10.0, [&]() { return not w.of(0, "FishingShip").empty(); }),
	      "fishing ship trained");
	auto ships = w.of(0, "FishingShip");
	if (ships.empty()) {
		return;
	}
	auto ship = ships.front();
	check(w.gather(ship)->ship, "fishing ship moves on water");
	std::shared_ptr<GameEntity> fish;
	for (const auto &name : {"Shorefish", "BigOceanFish"}) {
		for (const auto &f : w.named(name)) {
			if (fish == nullptr or w.distance(f, dock) < w.distance(fish, dock)) {
				fish = f;
			}
		}
	}
	check(fish != nullptr, "fish on the map (" + std::to_string(fish ? w.distance(fish, dock) : 0.0) + " tiles from the dock)");
	if (fish == nullptr) {
		return;
	}
	check(not econ::can_gather_from(villagers.front(), fish), "villagers do not fish");
	check(econ::can_gather_from(ship, fish), "the fishing ship fishes");
	const double food0 = w.resource(0, resource_t::FOOD);
	w.gather_from(ship, fish);
	check(w.advance_until(240.0, [&]() { return w.resource(0, resource_t::FOOD) > food0 + 10.0; }),
	      "fish dropped at the dock (+" + std::to_string(w.resource(0, resource_t::FOOD) - food0) + " food)");
	// fishing trap on water, built by the ship (the data unlocks it in the Feudal Age: test grant)
	w.production->grant_research(0, "FeudalAge");
	w.advance(1.0);
	w.select({ship});
	snap = w.production->snapshot();
	check(w.option(snap, "FishingTrap") != nullptr, "the ship builds fishing traps");
	auto sp = w.position(ship);
	auto trap = w.place_near(0, {ship}, "FishingTrap", sp.ne.to_double() + 2.0, sp.se.to_double(), 6);
	check(trap != nullptr, "fishing trap placed on water");
	if (trap == nullptr) {
		return;
	}
	auto tp = w.position(trap);
	check(ends_with(w.terrain_at(tp.ne.to_double(), tp.se.to_double()), "Water")
	          or ends_with(w.terrain_at(tp.ne.to_double(), tp.se.to_double()), "Water2")
	          or ends_with(w.terrain_at(tp.ne.to_double(), tp.se.to_double()), "Water3"),
	      "trap stands on water: " + w.terrain_at(tp.ne.to_double(), tp.se.to_double()));
	check(w.advance_until(120.0, [&]() { return prod::is_finished(trap); }), "fishing trap built by the ship");
	auto th = w.harvestable(trap);
	check(th != nullptr and th->get_amount() > 100.0, "trap food of the data: " + std::to_string(th ? th->get_amount() : 0.0));
	w.advance(30.0);
	auto target = w.gather(ship)->get_job().target;
	check(target and *target == trap->get_id() and th->get_amount() < th->get_start_amount(),
	      "the ship fishes at its trap (" + std::to_string(th->get_amount()) + " left)");
}

// ------------------------------------------------------------------ F9

void check_save(const Options &opt) {
	std::cout << "== F9 Spielstand Version 4" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.vision = vision_mode_t::ALL;
	World w{opt, base};
	w.advance(1.0);
	w.add_resources(0, 2000.0);
	auto tc = w.town_center(0);
	auto tcp = w.position(tc);
	auto villagers = w.of(0, "Villager");
	auto mill = w.place_near(0, {villagers[0]}, "Mill", tcp.ne.to_double() - 5.0, tcp.se.to_double() + 5.0);
	w.advance_until(150.0, [&]() { return mill and prod::is_finished(mill); });
	auto farm = w.place_near(0, {villagers[1]}, "Farm", tcp.ne.to_double() + 5.0, tcp.se.to_double() - 1.0);
	w.advance_until(120.0, [&]() { return farm and prod::is_finished(farm); });
	w.select({mill});
	w.production->command(prod::CMD_RESEED_FARM);
	w.production->command(prod::CMD_RESEED_FARM);
	w.advance(0.5);
	auto sheep = w.of(0, "Sheep");
	check(sheep.size() >= 2, "own sheep for the save");
	if (sheep.size() < 2) {
		return;
	}
	w.gather_from(villagers[2], sheep.front());
	auto sh = w.harvestable(sheep.front());
	w.advance_until(30.0, [&]() { return sh->dead; });
	w.advance(3.0);
	check(sh->dead, "carcass for the save");
	auto data = save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "W3");
	const auto text = save::serialize(data);
	check(text.find("version = 4") != std::string::npos and text.find("herds = 1") != std::string::npos,
	      "save version 4 with herds");
	check(text.find("dead = 1") != std::string::npos, "carcass stored (dead)");
	check(text.find("reseed = ") != std::string::npos, "reseeds stored");
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parsed: " + error);
	// load into a new game of the same map
	MapSettings again = w.settings;
	World l{opt, again};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	check(result.ok, "restored: " + result.error);
	l.advance(1.0);
	check(l.food()->reseeds(0) == 2, "reseeds restored (" + std::to_string(l.food()->reseeds(0)) + ")");
	size_t carcasses = 0;
	for (const auto &s : l.named("Sheep", false)) {
		auto h = l.harvestable(s);
		carcasses += h and h->dead ? 1 : 0;
	}
	check(carcasses == 1, "carcass restored");
	size_t own = l.of(0, "Sheep").size();
	check(own + carcasses >= sheep.size(), "converted sheep restored (" + std::to_string(own) + " own)");
	auto farms = l.of(0, "Farm");
	check(farms.size() == 1 and prod::is_finished(farms.front()), "farm restored");
	if (not farms.empty()) {
		auto p = l.position(farms.front());
		check(ends_with(l.terrain_at(p.ne.to_double(), p.se.to_double()), ".FarmCrops"), "farm terrain after loading");
	}
	auto again_data = save::capture(l.state, again, l.game->get_generated_entity_range(), l.now, "W3");
	auto a = save::capture(w.state, w.settings, w.game->get_generated_entity_range(), w.now, "W3");
	// same entity count of the food sources
	auto count_food = [](const save::SaveData &d) {
		size_t n = 0;
		for (const auto &e : d.entities) {
			n += e.dead ? 1 : 0;
		}
		return n;
	};
	check(count_food(again_data) == count_food(a), "carcasses after save -> load -> save");
	// version 3 file: no animals on its map
	std::string v3 = text;
	auto pos = v3.find("version = 4");
	v3.replace(pos, 11, "version = 3");
	save::SaveData old;
	check(save::parse(v3, old, error) and not old.map.herds, "version 3 file parses, map without animals");
}

// ------------------------------------------------------------------ F10

void check_ai(const Options &opt) {
	std::cout << "== F10 Computer-Gegner: Mühle, Farmen, Schafe, Feudalzeit" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::ON;
	base.ai.player = 1;
	base.ai.first_attack = 100000.0;
	base.civs = {"Britons", "Franks"};
	World w{opt, base};
	const std::string feudal_age = "hd_base.data.tech.generic.feudal_age.feudal_age.FeudalAge";
	double t_mill = -1.0;
	double t_farm = -1.0;
	double t_feudal = -1.0;
	size_t max_farms = 0;
	double food_at_feudal = 0.0;
	while (w.now.to_double() < opt.ai_limit and (t_feudal < 0.0 or max_farms < 2)) {
		w.advance(5.0);
		if (t_mill < 0.0) {
			for (const auto &m : w.of(1, "Mill")) {
				if (prod::is_finished(m)) {
					t_mill = w.now.to_double();
				}
			}
		}
		size_t farms = 0;
		for (const auto &f : w.of(1, "Farm")) {
			farms += prod::is_finished(f) ? 1 : 0;
		}
		max_farms = std::max(max_farms, farms);
		if (t_farm < 0.0 and farms > 0) {
			t_farm = w.now.to_double();
		}
		const auto *s = w.state->get_tech()->find(1);
		if (t_feudal < 0.0 and s and s->age >= 2) {
			t_feudal = w.now.to_double();
			food_at_feudal = w.resource(1, resource_t::FOOD);
		}
	}
	auto c = w.food()->counters();
	std::cout << "  AI: mill t=" << t_mill << " s, first farm t=" << t_farm << " s, farms " << max_farms
	          << ", Feudal Age t=" << t_feudal << " s (0.6.0-xr.0.14: 1120 s), carcasses " << c.carcasses
	          << ", conversions " << c.conversions << std::endl;
	check(t_mill > 0.0, "AI built a mill");
	check(t_farm > 0.0 and max_farms >= 1, "AI built farms (" + std::to_string(max_farms) + ")");
	check(c.carcasses >= 2, "AI slaughtered sheep or hunted (" + std::to_string(c.carcasses) + " carcasses)");
	check(t_feudal > 0.0 and t_feudal < 1120.0, "AI reached the Feudal Age earlier than 18:40: t=" + std::to_string(t_feudal));
	(void)food_at_feudal;
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
			std::cerr << "usage: openage-food-game-check --root <dir> [--modpack id] [--seed n] [--coast-seed n] [--size n]"
			             " [--ai-limit s] [--only animals|farms|fishing|save|ai]"
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
		if (only.empty() or only == "animals") {
			check_animals(opt);
		}
		if (only.empty() or only == "farms") {
			check_farms(opt);
		}
		if (only.empty() or only == "fishing") {
			check_fishing(opt);
		}
		if (only.empty() or only == "save") {
			check_save(opt);
		}
		if (only.empty() or only == "ai") {
			check_ai(opt);
		}
	}
	catch (std::exception &err) {
		std::cout << "food game check FAILED: " << err.what() << std::endl;
		return 1;
	}
	if (failures == 0) {
		std::cout << "food game check ok (" << passed << " checks)" << std::endl;
		return 0;
	}
	std::cout << "food game check: " << failures << " failures, " << passed << " ok" << std::endl;
	return 1;
}
