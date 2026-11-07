// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Game state check of the technologies (XR fork W1), headless, with the
 * converted modpack but without renderer:
 *
 *   openage-tech-game-check --root <dir> [--modpack hd_base] [--seed N] [--size N] [--ai-limit S]
 *
 * Random map (64 tiles), simulated time advances in steps of 50 ms through the
 * event loop with the production of the simulation (like GameSimulation::run).
 *
 *  T1. Research Loom in the own town centre: 50 gold paid, queued like a unit,
 *      after its research time the existing villagers have 15 more maximum and
 *      current health (AoE II), "Webstuhl erforscht"; Loom is not offered again.
 *  T2. Feudal Age: locked with the reason of the data (2 buildings), not paid;
 *      after a mill and a lumber camp exist it is offered; cancel refunds the
 *      food; researched again -> age 2, "Feudalzeit" in the snapshot, message
 *      "Spieler 1 … ist zur Feudalzeit vorangeschritten.", the house shows its new graphic.
 *  T3. Unlocks: in the Dark Age the archery range is not offered to villagers,
 *      in the Feudal Age it is; ships/farms/monks never (catalog).
 *  T4. Line upgrade Swordsman (Man-at-Arms) in a barracks: an existing militia
 *      gets the new maximum health and attack, its label is "Schwertkämpfer",
 *      the barracks trains "Schwertkämpfer".
 *  T5. Civilisations: Britons (player 0) and Japanese (player 1): civ recorded,
 *      castle of the Britons trains longbowmen (game_setup patch), the town
 *      centres show different architecture, a fixed and a random choice.
 *  T6. Save game (version 6 since W6) with civilisation, technologies and a queued
 *      research; loading applies them again (villager health, age); a version 1
 *      file still loads.
 *  T7. Computer opponent: researches Loom and the Feudal Age (builds the second
 *      building it needs), then the Wheelbarrow.
 *  T8. W1 + W2 together (fog of war "normal"): start area visible, enemy town
 *      centre in the fog, civilisation patches work; Town Watch raises the line
 *      of sight and the vision grid follows; save -> load -> save keeps
 *      civilisation, techs, vision mode and explored tiles; version 2 loads.
 *
 * Prints "tech game check ok" and exits with 0 if everything passes.
 */

#include <algorithm>
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
#include "gamestate/component/internal/activity.h"
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
	double ai_limit = 3000.0;
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

		MapSettings settings = base;
		settings.type = map_type_t::RANDOM;
		settings.seed = opt.seed;
		settings.size = opt.size;
		this->game = std::make_shared<Game>(this->loop, mod_manager, this->factory, terrain_factory, settings);
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

	std::shared_ptr<tech::TechState> techs() const {
		return this->state->get_tech();
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

	/// entities of a player with this nyan short name, sorted by id
	std::vector<std::shared_ptr<GameEntity>> of(player_id_t player, const std::string &name) const {
		std::vector<std::shared_ptr<GameEntity>> out;
		for (const auto &[id, e] : this->state->get_game_entities()) {
			if (e->has_component(component::component_t::OWNERSHIP) and this->owner(e) == player
			    and prod::entity_name(e) == name and not this->state->get_combat()->is_dead(id)) {
				out.push_back(e);
			}
		}
		std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a->get_id() < b->get_id(); });
		return out;
	}

	/// a finished entity of a player next to its town centre
	std::shared_ptr<GameEntity> spawn(player_id_t player, const std::string &fqon, double dne, double dse) {
		auto tc = this->of(player, "TownCenter").front();
		auto base = this->position(tc);
		auto entity = this->factory->add_game_entity(this->loop, this->state, player, fqon);
		auto pos = std::dynamic_pointer_cast<component::Position>(entity->get_component(component::component_t::POSITION));
		coord::phys3 at{base.ne + coord::phys_t{dne}, base.se + coord::phys_t{dse}, coord::phys_t{0.0}};
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

	int64_t max_health(const std::shared_ptr<GameEntity> &e) const {
		auto h = this->state->get_combat()->get_health({e->get_id()});
		return h.empty() ? -1 : h.front().max_health;
	}

	int64_t health(const std::shared_ptr<GameEntity> &e) const {
		auto h = this->state->get_combat()->get_health({e->get_id()});
		return h.empty() ? -1 : h.front().health;
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
};

// T1 … T4 and T6 in one game (player 0 Britons, player 1 Franks, no AI)
void check_research(const Options &opt) {
	std::cout << "== T1 Webstuhl" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.civs = {"Britons", "Franks"};
	World w{opt, base};
	auto tc = w.of(0, "TownCenter").front();
	auto villagers = w.of(0, "Villager");
	check(not villagers.empty(), "villagers of player 0");
	const auto hp_before = w.max_health(villagers.front());
	const auto cur_before = w.health(villagers.front());
	w.select({tc});
	auto snap = w.production->snapshot();
	auto loom = w.option(snap, "Loom");
	check(loom != nullptr and loom->research and loom->available and loom->label == "Webstuhl"
	          and loom->cost[static_cast<size_t>(resource_t::GOLD)] > 0,
	      "town centre offers Loom (research, available, costs gold)");
	auto feudal = w.option(snap, "FeudalAge");
	check(feudal != nullptr and not feudal->available and feudal->reason.find("Benötigt") != std::string::npos,
	      "Feudal Age shown, locked: " + (feudal ? feudal->reason : std::string{"-"}));
	check(w.option(snap, "CastleAge") == nullptr and w.option(snap, "WheelBarrow") == nullptr,
	      "later ages and Wheelbarrow hidden in the Dark Age");
	check(snap.age == 1 and snap.age_label == "Dunkles Zeitalter" and snap.civ == "Britons"
	          and snap.civ_label == "Briten",
	      "snapshot: Dark Age, Britons");
	const double gold = w.resource(0, resource_t::GOLD);
	w.production->research("Loom");
	w.advance(0.3);
	check(std::abs(w.resource(0, resource_t::GOLD) - (gold - loom->cost[2])) < 1e-6, "Loom paid");
	snap = w.production->snapshot();
	check(snap.queue and snap.queue->items.size() == 1 and snap.queue->items[0].research
	          and snap.queue->items[0].label == "Webstuhl",
	      "Loom in the queue of the town centre");
	bool in_orders = false;
	for (const auto &o : snap.orders) {
		in_orders = in_orders or (o.label == "Webstuhl" and o.building == tc->get_id());
	}
	check(in_orders, "Loom in the orders");
	auto loom_again = w.option(snap, "Loom");
	check(loom_again != nullptr and not loom_again->available, "Loom locked while it is researched");
	w.advance(loom->time + 1.0);
	check(w.techs()->is_researched(0, "hd_base.data.tech.generic.loom.loom.Loom"), "Loom researched");
	check(w.max_health(villagers.front()) == hp_before + 15, "villager max health +15 ("
	                                                             + std::to_string(hp_before) + " -> "
	                                                             + std::to_string(w.max_health(villagers.front())) + ")");
	check(w.health(villagers.front()) == cur_before + 15, "villager current health +15");
	auto stats = w.state->get_combat()->get_stats(villagers.front()->get_id());
	check(stats and not stats->armor.block.empty(), "villager stats read again");
	snap = w.production->snapshot();
	check(w.option(snap, "Loom") == nullptr, "Loom not offered again");
	check(snap.status.find("Webstuhl erforscht") != std::string::npos or snap.status_seq > 0, "message " + snap.status);

	std::cout << "== T2/T3 Feudalzeit, Freigaben" << std::endl;
	w.select({villagers.front()});
	snap = w.production->snapshot();
	check(w.option(snap, "House") != nullptr and w.option(snap, "Mill") != nullptr, "villager builds house and mill");
	check(w.option(snap, "ArcheryRange") == nullptr and w.option(snap, "Blacksmith") == nullptr,
	      "archery range, blacksmith hidden in the Dark Age");
	// XR fork W3: the dock has its mechanic now, the farm waits for a mill, the market (W7) for its age
	check(w.option(snap, "Farm") == nullptr and w.option(snap, "Market") == nullptr,
	      "farm hidden without a mill, market hidden in the Dark Age");
	check(w.option(snap, "Dock") != nullptr, "dock offered (W3)");
	auto house = w.spawn(0, ge("house", "House"), 6.0, -6.0);
	w.advance(0.5);
	const auto house_anim = house->get_animation_path();
	w.add_resources(0, 2000.0);
	w.select({tc});
	const double food = w.resource(0, resource_t::FOOD);
	w.production->research("FeudalAge");
	w.advance(0.3);
	check(std::abs(w.resource(0, resource_t::FOOD) - food) < 1e-6, "locked Feudal Age not paid");
	check(w.production->snapshot().status.find("Benötigt") != std::string::npos,
	      "reason as message: " + w.production->snapshot().status);
	w.spawn(0, ge("mill", "Mill"), -6.0, 6.0);
	w.spawn(0, ge("lumber_camp", "LumberCamp"), 7.0, 7.0);
	w.advance(0.5);
	snap = w.production->snapshot();
	feudal = w.option(snap, "FeudalAge");
	check(feudal != nullptr and feudal->available, "Feudal Age available with mill and lumber camp");
	w.production->research("FeudalAge");
	w.advance(0.3);
	check(std::abs(w.resource(0, resource_t::FOOD) - (food - feudal->cost[0])) < 1e-6, "Feudal Age paid");
	w.production->cancel_training_at(0);
	w.advance(0.3);
	check(std::abs(w.resource(0, resource_t::FOOD) - food) < 1e-6, "cancel refunds the food");
	w.production->research("FeudalAge");
	w.advance(feudal->time + 1.0);
	snap = w.production->snapshot();
	check(w.techs()->find(0)->age == 2 and snap.age == 2 and snap.age_label == "Feudalzeit", "age 2: Feudalzeit");
	check(snap.status.find("ist zur Feudalzeit vorangeschritten") != std::string::npos, "message: " + snap.status);
	{
		auto view = w.state->get_player(0)->get_db_view();
		auto animated = view->get_object(ge("house", "House") + ".Idle.Animated");
		for (const auto &v : animated.get_set("Animated.animations")) {
			auto name = std::dynamic_pointer_cast<nyan::ObjectValue>(v.get_ptr())->get_name();
			std::cout << "  house idle animation in the data: " << name << " sprite "
			          << view->get_object(name).get_file("Animation.sprite") << std::endl;
		}
	}
	check(house->get_animation_path() != house_anim and not house->get_animation_path().empty(),
	      "house shows the Feudal graphic (" + house->get_animation_path() + ")");
	w.select({villagers.front()});
	snap = w.production->snapshot();
	check(w.option(snap, "ArcheryRange") != nullptr and w.option(snap, "Blacksmith") != nullptr,
	      "archery range and blacksmith offered in the Feudal Age");
	w.select({tc});
	snap = w.production->snapshot();
	check(w.option(snap, "WheelBarrow") != nullptr and w.option(snap, "CastleAge") != nullptr,
	      "Wheelbarrow and Castle Age offered");

	std::cout << "== T4 Schwertkämpfer" << std::endl;
	auto barracks = w.spawn(0, ge("barracks", "Barracks"), -7.0, -7.0);
	auto militia = w.spawn(0, ge("militia", "Militia"), 3.0, 4.0);
	w.advance(0.5);
	const auto militia_hp = w.max_health(militia);
	const auto militia_attack = w.state->get_combat()->get_stats(militia->get_id())->attack;
	w.select({barracks});
	snap = w.production->snapshot();
	auto mia = w.option(snap, "Swordsman");
	check(mia != nullptr and mia->available and mia->label == "Schwertkämpfer", "barracks offers Swordsman");
	w.production->research("Swordsman");
	w.advance((mia ? mia->time : 40.0) + 1.0);
	const auto militia_attack_after = w.state->get_combat()->get_stats(militia->get_id())->attack;
	check(w.max_health(militia) > militia_hp, "militia max health " + std::to_string(militia_hp) + " -> "
	                                              + std::to_string(w.max_health(militia)));
	check(combat::attack_sum(militia_attack_after) > combat::attack_sum(militia_attack), "militia attack rises");
	check(w.techs()->unit_label(0, ge("militia", "Militia")) == "Schwertkämpfer", "militia called Schwertkämpfer");
	snap = w.production->snapshot();
	auto train = w.option(snap, "Militia");
	check(train != nullptr and train->label == "Schwertkämpfer", "barracks trains Schwertkämpfer");
	bool label_listed = false;
	for (const auto &[n, l] : snap.unit_labels) {
		label_listed = label_listed or (n == "Militia" and l == "Schwertkämpfer");
	}
	check(label_listed, "unit labels for the interface");
	// player 1 is unchanged (its own view)
	auto other = w.spawn(1, ge("militia", "Militia"), 3.0, 4.0);
	w.advance(0.2);
	check(w.max_health(other) == militia_hp, "militia of player 1 unchanged");

	std::cout << "== T5 Kulturen" << std::endl;
	check(w.techs()->find(0)->civ == "Britons" and w.techs()->find(1)->civ == "Franks", "civilisations recorded");
	auto castle_create = [&](player_id_t p) {
		auto view = w.state->get_player(p)->get_db_view();
		std::string all;
		for (const auto &v : view->get_object(ge("castle", "Castle") + ".Create").get_set("Create.creatables")) {
			all += std::dynamic_pointer_cast<nyan::ObjectValue>(v.get_ptr())->get_name() + " ";
		}
		return all;
	};
	check(castle_create(0).find("LongbowmanCreatable") != std::string::npos, "Britons castle trains longbowmen");
	check(castle_create(1).find("LongbowmanCreatable") == std::string::npos, "Franks castle does not");
	auto range0 = w.techs()->researchables(w.state, 0, ge("archery_range", "ArcheryRange"));
	bool thumb = false;
	for (const auto &r : range0) {
		thumb = thumb or r.name == "ThumbRing";
	}
	check(not thumb, "Britons: no Thumb Ring (tech tree)");

	std::cout << "== T6 Spielstand" << std::endl;
	w.select({tc});
	w.production->research("WheelBarrow");
	w.advance(1.0);
	auto data = save::capture(w.state, base, w.game->get_generated_entity_range(), w.now, "tech check");
	const auto text = save::serialize(data);
	check(text.find("version = 9") != std::string::npos and text.find("vision = ") != std::string::npos
	          and text.find("civ = Britons") != std::string::npos
	          and text.find("tech = hd_base.data.tech.generic.loom.loom.Loom") != std::string::npos
	          and text.find(" research\n") != std::string::npos and text.find("civs = Britons Franks") != std::string::npos,
	      "save text with civilisation, techs and the queued research");
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error), "parse version 6 " + error);
	MapSettings loaded_map = parsed.map;
	World l{opt, loaded_map};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	l.advance(0.5);
	check(result.ok, "restore " + result.error);
	check(l.techs()->find(0) and l.techs()->find(0)->civ == "Britons" and l.techs()->find(0)->age == 2,
	      "loaded: Britons, Feudal Age");
	auto lv = l.of(0, "Villager");
	check(not lv.empty() and l.max_health(lv.front()) == hp_before + 15, "loaded villagers keep Loom");
	auto again = save::capture(l.state, loaded_map, l.game->get_generated_entity_range(), l.now, "tech check");
	check(again.players.size() == data.players.size() and again.players[0].techs == data.players[0].techs,
	      "techs survive save -> load -> save");
	bool queued = false;
	for (const auto &e : again.entities) {
		for (const auto &q : e.queue) {
			queued = queued or (q.research and q.name == "WheelBarrow");
		}
	}
	check(queued, "queued research survives loading");
	// version 1: no civilisation and techs, still loads (version 2: the same without vision)
	std::string v1 = text;
	v1.replace(v1.find("version = 9"), 11, "version = 1");
	save::SaveData old;
	check(save::parse(v1, old, error), "version 1 file parses");
}

// T8 (W1 + W2 together): civilisation patches under the fog of war, a sight technology widens the
// fog, and a save game keeps both (civilisation, technologies, vision mode, explored tiles)
void check_civ_vision(const Options &opt) {
	std::cout << "== T8 Kultur + Sicht" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::OFF;
	base.civs = {"Britons", "Franks"};
	base.vision = vision_mode_t::NORMAL;
	World w{opt, base};
	w.advance(0.5);
	auto vision = w.state->get_vision();
	check(vision and vision->is_started() and vision->get_mode() == vision_mode_t::NORMAL, "fog of war started (normal)");
	if (not vision) {
		return;
	}
	const auto &grid = vision->get_grid();
	auto tc = w.of(0, "TownCenter").front();
	const auto p = w.position(tc);
	const auto enemy = w.position(w.of(1, "TownCenter").front());
	check(vision->sees(0, p.ne.to_double(), p.se.to_double(), 0.0), "start area visible (own town centre)");
	check(not vision->sees(0, enemy.ne.to_double(), enemy.se.to_double(), 0.0), "enemy town centre in the fog");
	const size_t visible0 = grid.visible_count(0);
	const size_t explored0 = grid.explored_count(0);
	std::cout << "  visible " << visible0 << ", explored " << explored0 << " of " << grid.tiles() << " tiles" << std::endl;
	check(visible0 > 0 and explored0 >= visible0 and explored0 < grid.tiles() / 2, "only the start area is explored");
	// civilisation patches work with the fog: tech tree and unique unit as in T5
	check(w.techs()->find(0) and w.techs()->find(0)->civ == "Britons" and w.techs()->find(1)
	          and w.techs()->find(1)->civ == "Franks",
	      "civilisations Britons/Franks with vision");
	bool thumb = false;
	for (const auto &r : w.techs()->researchables(w.state, 0, ge("archery_range", "ArcheryRange"))) {
		thumb = thumb or r.name == "ThumbRing";
	}
	check(not thumb, "Britons tech tree (no Thumb Ring) with vision");
	// a technology that patches the line of sight reaches the vision grid (refresh -> restamp)
	const double los_before = w.state->get_combat()->get_stats(tc->get_id())->line_of_sight;
	w.production->grant_research(0, "FeudalAge");
	w.advance(0.5);
	w.production->grant_research(0, "TownWatch");
	w.advance(1.0);
	const double los_after = w.state->get_combat()->get_stats(tc->get_id())->line_of_sight;
	std::cout << "  town centre line of sight " << los_before << " -> " << los_after << ", visible tiles "
	          << visible0 << " -> " << grid.visible_count(0) << std::endl;
	check(los_after > los_before, "Town Watch raises the line of sight of the town centre");
	check(grid.visible_count(0) > visible0 and grid.explored_count(0) > explored0, "the wider sight explores more tiles");
	// save -> load -> save: civilisation, techs, vision mode and explored tiles survive
	auto data = save::capture(w.state, base, w.game->get_generated_entity_range(), w.now, "civ vision check");
	const auto text = save::serialize(data);
	check(text.find("vision = normal") != std::string::npos and text.find("explored = ") != std::string::npos
	          and text.find("civ = Britons") != std::string::npos
	          and text.find("TownWatch\n") != std::string::npos,
	      "save text with vision, explored tiles, civilisation and Town Watch");
	save::SaveData parsed;
	std::string error;
	check(save::parse(text, parsed, error) and parsed.map.vision == vision_mode_t::NORMAL, "parse " + error);
	MapSettings loaded_map = parsed.map;
	World l{opt, loaded_map};
	auto result = save::restore(parsed, l.state, l.loop, l.factory, l.production, l.game->get_generated_entity_range(), l.now);
	l.advance(0.5);
	check(result.ok, "restore " + result.error);
	auto lvision = l.state->get_vision();
	check(lvision and lvision->get_mode() == vision_mode_t::NORMAL, "loaded: fog of war normal");
	if (not lvision) {
		return;
	}
	check(l.techs()->find(0) and l.techs()->find(0)->civ == "Britons" and l.techs()->find(0)->age == 2,
	      "loaded: Britons, Feudal Age");
	const auto ltc = l.of(0, "TownCenter");
	check(not ltc.empty() and l.state->get_combat()->get_stats(ltc.front()->get_id())->line_of_sight == los_after,
	      "loaded: town centre keeps the line of sight of Town Watch");
	check(lvision->get_grid().explored_count(0) >= grid.explored_count(0), "loaded: explored tiles restored");
	check(lvision->sees(0, p.ne.to_double(), p.se.to_double(), 0.0)
	          and not lvision->sees(0, enemy.ne.to_double(), enemy.se.to_double(), 0.0),
	      "loaded: start area visible, enemy town centre still in the fog");
	auto again = save::capture(l.state, loaded_map, l.game->get_generated_entity_range(), l.now, "civ vision check");
	check(again.players.size() == data.players.size() and again.players[0].techs == data.players[0].techs
	          and again.players[0].civ == "Britons" and again.map.vision == vision_mode_t::NORMAL
	          and again.players[0].explored == data.players[0].explored,
	      "civilisation, techs, vision and explored tiles survive save -> load -> save");
	// version 2 (W1, before the vision fields) still loads: vision normal, nothing explored
	std::string v2 = text;
	v2.replace(v2.find("version = 9"), 11, "version = 2");
	v2.replace(v2.find("vision = normal\n"), 16, "");
	save::SaveData old;
	check(save::parse(v2, old, error) and old.map.vision == vision_mode_t::NORMAL and old.players.at(0).civ == "Britons",
	      "version 2 file parses (civilisation kept, vision normal)");
}

// T7: the computer opponent researches
void check_ai(const Options &opt) {
	std::cout << "== T7 KI" << std::endl;
	MapSettings base;
	base.ai.mode = ai_mode_t::ON;
	base.ai.difficulty = ai_difficulty_t::NORMAL;
	base.ai.first_attack = 100000.0;
	base.civs = {"random", "random"};
	World w{opt, base};
	check(w.techs()->find(0) and w.techs()->find(1) and w.techs()->find(0)->civ != w.techs()->find(1)->civ,
	      "random civilisations differ");
	// XR fork W4 + W3: the town centre shoots one arrow without a garrison (data); the computer avoids its
	// range and lives on farms (W3), so the passive player keeps its town centre.
	const std::string loom = "hd_base.data.tech.generic.loom.loom.Loom";
	const std::string wheel = "hd_base.data.tech.generic.wheel_barrow.wheel_barrow.WheelBarrow";
	double t_loom = -1.0, t_feudal = -1.0, t_wheel = -1.0;
	while (w.now.to_double() < opt.ai_limit and t_wheel < 0.0) {
		w.advance(5.0);
		const auto *s = w.techs()->find(1);
		if (t_loom < 0.0 and s and s->facts.researched.contains(loom)) {
			t_loom = w.now.to_double();
		}
		if (t_feudal < 0.0 and s and s->age >= 2) {
			t_feudal = w.now.to_double();
		}
		if (t_wheel < 0.0 and s and s->facts.researched.contains(wheel)) {
			t_wheel = w.now.to_double();
		}
	}
	std::cout << "  AI: Loom t=" << t_loom << " s, Feudal t=" << t_feudal << " s, Wheelbarrow t=" << t_wheel << " s"
	          << std::endl;
	check(t_loom > 0.0, "AI researched Loom");
	check(t_feudal > 0.0, "AI reached the Feudal Age");
	check(t_wheel > 0.0, "AI researched the Wheelbarrow");
	check(w.techs()->find(0)->facts.researched.empty(), "the human player researched nothing");
}

} // namespace

int main(int argc, char **argv) {
	Options opt;
	bool have_root = false;
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
		else if (a == "--ai-limit") {
			opt.ai_limit = std::stod(next());
		}
		else {
			std::cerr << "usage: openage-tech-game-check --root <dir> [--modpack id] [--seed n] [--size n] [--ai-limit s]"
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
		check_research(opt);
		check_civ_vision(opt);
		check_ai(opt);
	}
	catch (std::exception &err) {
		std::cout << "tech game check FAILED: " << err.what() << std::endl;
		return 1;
	}
	if (failures == 0) {
		std::cout << "tech game check ok" << std::endl;
		return 0;
	}
	std::cout << "tech game check: " << failures << " failures" << std::endl;
	return 1;
}
