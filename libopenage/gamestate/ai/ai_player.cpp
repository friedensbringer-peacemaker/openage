// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "ai_player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

#include "event/event_loop.h"
#include "log/log.h"
#include "log/message.h"

#include "assets/mod_manager.h"
#include "coord/tile.h"
#include "gamestate/ai/events.h"
#include "gamestate/ai/production_port.h"
#include "gamestate/tech.h"
#include "gamestate/tech_math.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/build.h"
#include "gamestate/component/internal/commands/gather.h"
#include "gamestate/component/internal/commands/move.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/econ.h"
#include "gamestate/food.h"
#include "gamestate/food_math.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"
#include "gamestate/player.h"
#include "gamestate/production.h"
#include "gamestate/production_math.h"
#include "gamestate/special.h"
#include "gamestate/vision/vision_state.h"


namespace openage::gamestate::ai {

namespace {

time::time_t seconds(double s) {
	return time::time_t::from_double(s);
}

double dist(double ane, double ase, double bne, double bse) {
	return std::hypot(ane - bne, ase - bse);
}

std::optional<gather_t> gather_kind(resource_t resource) {
	switch (resource) {
	case resource_t::FOOD:
		return gather_t::FOOD;
	case resource_t::WOOD:
		return gather_t::WOOD;
	case resource_t::GOLD:
		return gather_t::GOLD;
	default:
		return std::nullopt;
	}
}

std::string fmt1(double v) {
	std::ostringstream out;
	out.setf(std::ios::fixed);
	out.precision(1);
	out << v;
	return out.str();
}

template <typename T>
std::shared_ptr<T> component_of(const std::shared_ptr<GameEntity> &entity, component::component_t type) {
	if (entity == nullptr or not entity->has_component(type)) {
		return nullptr;
	}
	return std::dynamic_pointer_cast<T>(entity->get_component(type));
}

} // namespace


AiPlayer::AiPlayer(const std::shared_ptr<openage::event::EventLoop> &loop,
                   player_id_t player,
                   const std::unordered_set<player_id_t> &neutral,
                   const AiParams &params,
                   uint64_t seed,
                   const std::shared_ptr<ProductionPort> &production) :
	openage::event::EventEntity{loop},
	loop{loop},
	handler{std::make_shared<AiEventHandler>()},
	player{player},
	neutral{neutral},
	params{params},
	rng{seed},
	production{production} {
	this->status.player = player;
	this->status.difficulty = params.difficulty;
	this->plan_log = LogThrottle{params.report_period};
}

size_t AiPlayer::id() const {
	// fixed id per player ("ai" + player)
	return 0x61690000 + static_cast<size_t>(this->player);
}

std::string AiPlayer::idstr() const {
	return "ai" + std::to_string(this->player);
}

player_id_t AiPlayer::get_player() const {
	return this->player;
}

const AiParams &AiPlayer::get_params() const {
	return this->params;
}

AiStatus AiPlayer::get_status() const {
	std::lock_guard<std::mutex> lock{this->status_mutex};
	return this->status;
}

void AiPlayer::set_production(const std::shared_ptr<ProductionPort> &production) {
	if (production) {
		this->production = production;
		log::log(INFO << "AI P" << this->player << ": production " << production->name());
	}
}

void AiPlayer::start(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	if (this->started) {
		return;
	}
	this->started = true;
	this->running = true;
	{
		std::lock_guard<std::mutex> lock{this->status_mutex};
		this->status.running = true;
	}
	log::log(INFO << "AI P" << this->player << ": computer opponent started (" << to_string(this->params.difficulty)
	              << ", reaction " << this->params.think_period << " s, attack with "
	              << this->params.attack_threshold << " units, first attack at t>="
	              << this->params.first_attack_earliest << " s, any army after "
	              << this->params.attack_anyway_after << " s, production: " << this->production->name() << ")");
	this->next_report = time.to_double() + this->params.report_period;
	this->schedule(state, time + seconds(this->params.think_period));
}

void AiPlayer::schedule(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	this->loop->create_event(this->handler,
	                         this->shared_from_this(),
	                         state,
	                         time,
	                         openage::event::EventHandler::param_map::map_t{});
}

// ---------------------------------------------------------------- decision round

void AiPlayer::think(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	if (not this->running) {
		return;
	}
	auto combat = state->get_combat();
	auto match = combat->get_match_status();
	if (match.result.over) {
		this->running = false;
		log::log(INFO << "AI P" << this->player << ": match over at t=" << fmt1(time.to_double()) << " s, result "
		              << combat::to_string(combat->get_match_state(this->player)) << ", waves "
		              << this->waves);
		std::lock_guard<std::mutex> lock{this->status_mutex};
		this->status.running = false;
		this->status.phase = "over";
		return;
	}

	auto world = this->observe(state, time);
	if (world.villagers.empty() and world.army.empty() and world.buildings.empty()) {
		this->running = false;
		log::log(INFO << "AI P" << this->player << ": no units or buildings left at t="
		              << fmt1(time.to_double()) << " s, stops");
		std::lock_guard<std::mutex> lock{this->status_mutex};
		this->status.running = false;
		this->status.phase = "over";
		return;
	}

	this->defend(state, world, time);
	this->attack(state, world, time);
	// XR fork W5: repairs before the economy takes the free villagers
	this->repair(state, world, time);
	this->gather(state, world, time);
	this->produce(state, world, time);
	this->food_economy(state, world, time);
	this->fortify(state, world, time);
	this->special(state, world, time);
	this->report(state, world, time);

	{
		std::lock_guard<std::mutex> lock{this->status_mutex};
		this->status.thinks += 1;
		this->status.waves = this->waves;
		this->status.villagers = world.villagers.size();
		this->status.military = world.army.size();
		this->status.resources = state->get_player(this->player)->get_resources().get();
		this->status.phase = this->defending ? "defend" : (this->wave.empty() ? "economy" : "attack");
	}

	this->schedule(state, time + seconds(this->params.think_period));
}

AiPlayer::World AiPlayer::observe(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	World world;
	auto combat = state->get_combat();
	auto vision = state->get_vision();
	for (const auto &[id, entity] : state->get_game_entities()) {
		auto stats = combat->get_stats(id);
		// XR fork W3: herdables of a player are food, no units of the army or targets;
		// XR fork W4: units in a building (or on their way in) are neither seen nor given jobs
		if (not stats or combat->is_dead(id) or not stats->attackable() or stats->animal or combat->garrisoned_in(id)
		    or combat->entering(id)
		    or not entity->has_component(component::component_t::OWNERSHIP)
		    or not entity->has_component(component::component_t::POSITION)) {
			continue;
		}
		auto ownership = std::dynamic_pointer_cast<component::Ownership>(
			entity->get_component(component::component_t::OWNERSHIP));
		auto owner = ownership->get_owners().get(time);
		if (this->neutral.contains(owner)) {
			continue;
		}
		auto position = std::dynamic_pointer_cast<component::Position>(
			entity->get_component(component::component_t::POSITION));
		auto pos = position->get_positions().get(time);

		Seen seen;
		seen.id = id;
		seen.owner = owner;
		seen.ne = pos.ne.to_double();
		seen.se = pos.se.to_double();
		seen.radius = stats->radius;
		seen.sight = std::max(stats->line_of_sight, stats->max_range);
		seen.name = stats->name;
		seen.building = stats->building;
		seen.villager = stats->villager;
		seen.military = stats->can_attack and stats->movable and not stats->villager;
		seen.town_center = stats->building and stats->name == "TownCenter";
		// XR fork W6: monks, siege and ships from the special values of the data
		const special::SpecialType *st = state->get_special() ? state->get_special()->type_of(state, entity, time)
		                                                      : nullptr;
		seen.siege = st != nullptr and st->siege;

		if (owner != this->player) {
			// vision (XR fork): enemies in sight and remembered buildings (start town centres) only
			if (vision and not vision->knows(this->player, id, seen.ne, seen.se, seen.building ? seen.radius : 0.0, seen.building)) {
				continue;
			}
			world.enemies.push_back(std::move(seen));
		}
		else if (st != nullptr and st->ship and not seen.building) {
			world.fleet.push_back(std::move(seen));
		}
		else if (st != nullptr and st->monk and not seen.building) {
			world.monks.push_back(std::move(seen));
		}
		else if (seen.building) {
			world.buildings.push_back(std::move(seen));
		}
		else if (seen.villager) {
			world.villagers.push_back(std::move(seen));
		}
		else if (seen.military) {
			world.army.push_back(std::move(seen));
		}
	}
	auto by_id = [](const Seen &a, const Seen &b) { return a.id < b.id; };
	std::sort(world.enemies.begin(), world.enemies.end(), by_id);
	std::sort(world.buildings.begin(), world.buildings.end(), by_id);
	std::sort(world.villagers.begin(), world.villagers.end(), by_id);
	std::sort(world.army.begin(), world.army.end(), by_id);
	std::sort(world.monks.begin(), world.monks.end(), by_id);
	std::sort(world.fleet.begin(), world.fleet.end(), by_id);

	// health changes of own entities (attacked since the last round)
	std::vector<const Seen *> own;
	for (const auto *list : {&world.buildings, &world.villagers, &world.army}) {
		for (const auto &s : *list) {
			own.push_back(&s);
		}
	}
	std::vector<entity_id_t> ids;
	ids.reserve(own.size());
	for (const auto *s : own) {
		ids.push_back(s->id);
	}
	auto health = combat->get_health(ids);
	std::unordered_map<entity_id_t, int64_t> now_health;
	for (size_t i = 0; i < own.size(); ++i) {
		now_health[own[i]->id] = health[i].health;
		auto before = this->last_health.find(own[i]->id);
		if (before != this->last_health.end() and health[i].health < before->second) {
			world.hurt.push_back(*own[i]);
		}
	}
	this->last_health = std::move(now_health);

	// home: town center, else the centre of the own entities
	for (const auto &b : world.buildings) {
		if (b.town_center) {
			world.home_ne = b.ne;
			world.home_se = b.se;
			world.has_home = true;
			break;
		}
	}
	if (not world.has_home and not own.empty()) {
		for (const auto *s : own) {
			world.home_ne += s->ne;
			world.home_se += s->se;
		}
		world.home_ne /= static_cast<double>(own.size());
		world.home_se /= static_cast<double>(own.size());
		world.has_home = true;
	}

	// forget orders of units that are gone
	std::erase_if(this->last_order, [&](const auto &entry) { return not this->last_health.contains(entry.first); });
	std::erase_if(this->quick_idle, [&](const auto &entry) { return not this->last_health.contains(entry.first); });
	return world;
}

std::vector<TargetCandidate> AiPlayer::candidates(const World &world, double ne, double se) const {
	std::vector<TargetCandidate> result;
	result.reserve(world.enemies.size());
	for (const auto &e : world.enemies) {
		TargetCandidate c;
		c.id = e.id;
		c.distance = std::max(0.0, dist(ne, se, e.ne, e.se) - e.radius);
		c.building = e.building;
		c.town_center = e.town_center;
		c.military = e.military;
		result.push_back(c);
	}
	return result;
}

const AiPlayer::Seen *AiPlayer::find_enemy(const World &world, entity_id_t id) const {
	for (const auto &e : world.enemies) {
		if (e.id == id) {
			return &e;
		}
	}
	return nullptr;
}

bool AiPlayer::order_attack(const std::shared_ptr<GameState> &state,
                            const Seen &unit,
                            const Seen &target,
                            const time::time_t &time) {
	const double now = time.to_double();
	auto last = this->last_order.find(unit.id);
	if (last != this->last_order.end() and now - last->second < this->params.unit_order_cooldown) {
		return false;
	}
	this->last_order[unit.id] = now;
	if (not state->get_combat()->order_attack(state, unit.id, target.id, time)) {
		return false;
	}
	std::lock_guard<std::mutex> lock{this->status_mutex};
	this->status.attack_orders += 1;
	return true;
}

bool AiPlayer::order_attack_move(const std::shared_ptr<GameState> &state,
                                 const Seen &unit,
                                 double ne,
                                 double se,
                                 const time::time_t &time) {
	const double now = time.to_double();
	auto last = this->last_order.find(unit.id);
	if (last != this->last_order.end() and now - last->second < this->params.unit_order_cooldown) {
		return false;
	}
	this->last_order[unit.id] = now;
	coord::phys3 point{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}};
	if (not state->get_combat()->order_attack_move(state, unit.id, state->get_map()->on_terrain(point), time)) {
		return false;
	}
	std::lock_guard<std::mutex> lock{this->status_mutex};
	this->status.attack_moves += 1;
	return true;
}

void AiPlayer::set_stances(const std::shared_ptr<GameState> &state, const World &world) {
	// XR fork W4 (docs/aoe2-spec 06 §6.2, 09): the army at home guards the base without chasing enemies
	// across the map (defensive), the attack waves fight everything on their way (aggressive)
	auto combat = state->get_combat();
	for (const auto &unit : world.army) {
		const auto wanted = this->wave.contains(unit.id) ? combat::stance_t::AGGRESSIVE : combat::stance_t::DEFENSIVE;
		if (combat->get_stance(unit.id) != wanted) {
			combat->set_stance(unit.id, wanted);
		}
	}
}

// ---------------------------------------------------------------- defence

void AiPlayer::defend(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	const double now = time.to_double();
	auto combat = state->get_combat();

	// enemy units near own buildings or villagers, or near own entities that were hit
	std::vector<const Seen *> threats;
	for (const auto &e : world.enemies) {
		if (e.building) {
			continue;
		}
		bool threat = false;
		for (const auto &b : world.buildings) {
			if (dist(e.ne, e.se, b.ne, b.se) - b.radius <= this->params.defense_radius) {
				threat = true;
				break;
			}
		}
		for (size_t i = 0; not threat and i < world.villagers.size(); ++i) {
			const auto &v = world.villagers[i];
			threat = dist(e.ne, e.se, v.ne, v.se) <= this->params.villager_guard_radius;
		}
		for (size_t i = 0; not threat and i < world.hurt.size(); ++i) {
			const auto &h = world.hurt[i];
			threat = dist(e.ne, e.se, h.ne, h.se) - h.radius <= e.sight + 1.0;
		}
		if (threat) {
			threats.push_back(&e);
		}
	}

	if (threats.empty()) {
		if (this->defending and now - this->last_threat > 10.0) {
			this->defending = false;
			log::log(INFO << "AI P" << this->player << ": base safe again at t=" << fmt1(now) << " s");
		}
		// XR fork W4: back to work once the base is safe (town bell off)
		if (this->bell_tc and not this->defending) {
			if (combat->bell_rings(*this->bell_tc)) {
				combat->ring_bell(state, *this->bell_tc, false, time);
			}
			log::log(INFO << "AI P" << this->player << ": town bell off, villagers back to work at t=" << fmt1(now) << " s");
			this->bell_tc.reset();
		}
		return;
	}
	// XR fork W4: enemy soldiers among the villagers: ring the town bell (docs/aoe2-spec 06 §6.4, 09)
	if (not this->bell_tc) {
		size_t near_villagers = 0;
		for (const auto *t : threats) {
			if (not t->military) {
				continue;
			}
			for (const auto &v : world.villagers) {
				if (dist(t->ne, t->se, v.ne, v.se) <= this->params.villager_guard_radius) {
					near_villagers += 1;
					break;
				}
			}
		}
		const Seen *tc = nullptr;
		for (const auto &b : world.buildings) {
			if (b.town_center and (tc == nullptr or dist(b.ne, b.se, world.home_ne, world.home_se)
			                                         < dist(tc->ne, tc->se, world.home_ne, world.home_se))) {
				tc = &b;
			}
		}
		if (near_villagers > 0 and tc != nullptr and combat->ring_bell(state, tc->id, true, time)) {
			this->bell_tc = tc->id;
			{
				std::lock_guard<std::mutex> lock{this->status_mutex};
				this->status.bells += 1;
			}
			log::log(INFO << "AI P" << this->player << ": town bell rings (" << near_villagers
			              << " enemy soldiers near the villagers) at t=" << fmt1(now) << " s");
		}
	}
	this->last_threat = now;
	if (not this->defending) {
		this->defending = true;
		{
			std::lock_guard<std::mutex> lock{this->status_mutex};
			this->status.defenses += 1;
		}
		const auto *first = threats.front();
		log::log(INFO << "AI P" << this->player << ": defends against " << threats.size()
		              << " enemies near the base (" << first->name << " " << first->id << " (P" << first->owner
		              << ") at (" << fmt1(first->ne) << ", " << fmt1(first->se) << ")), "
		              << (world.army.size() - std::min(world.army.size(), this->wave.size()))
		              << " units at home, t=" << fmt1(now) << " s");
	}
	else if (this->defend_log.due(now)) {
		log::log(INFO << "AI P" << this->player << ": still defending against " << threats.size()
		              << " enemies, t=" << fmt1(now) << " s");
	}

	// army at home (wave units keep their objective) attacks the nearest threat
	std::vector<TargetCandidate> cands;
	for (const auto &unit : world.army) {
		if (this->wave.contains(unit.id)) {
			continue;
		}
		auto target = combat->get_target(unit.id);
		bool on_threat = false;
		for (const auto *t : threats) {
			on_threat = on_threat or (target and *target == t->id);
		}
		if (on_threat) {
			continue;
		}
		cands.clear();
		for (const auto *t : threats) {
			TargetCandidate c;
			c.id = t->id;
			c.distance = dist(unit.ne, unit.se, t->ne, t->se);
			c.military = t->military;
			cands.push_back(c);
		}
		auto pick = choose_unit_target(cands, 1e9);
		if (pick) {
			this->order_attack(state, unit, *threats[*pick], time);
		}
	}
}

// ---------------------------------------------------------------- attack waves

void AiPlayer::attack(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	const double now = time.to_double();
	auto combat = state->get_combat();
	// XR fork W4: stances of the army (home defensive, waves aggressive)
	this->set_stances(state, world);

	// running wave: drop dead units
	if (not this->wave.empty()) {
		std::set<entity_id_t> alive;
		for (const auto &unit : world.army) {
			if (this->wave.contains(unit.id)) {
				alive.insert(unit.id);
			}
		}
		if (alive.empty()) {
			log::log(INFO << "AI P" << this->player << ": attack wave " << this->waves
			              << " lost all units at t=" << fmt1(now) << " s");
		}
		this->wave = std::move(alive);
	}

	if (not this->wave.empty()) {
		// wave centre for the objective
		double cne = 0.0, cse = 0.0;
		size_t n = 0;
		for (const auto &unit : world.army) {
			if (this->wave.contains(unit.id)) {
				cne += unit.ne;
				cse += unit.se;
				n += 1;
			}
		}
		cne /= static_cast<double>(n);
		cse /= static_cast<double>(n);
		if (not this->objective or not this->find_enemy(world, *this->objective)) {
			auto cands = this->candidates(world, cne, cse);
			auto pick = choose_objective(cands);
			if (not pick) {
				log::log(INFO << "AI P" << this->player << ": attack wave " << this->waves
				              << " finds no enemies left at t=" << fmt1(now) << " s");
				this->wave.clear();
				this->objective.reset();
				return;
			}
			this->objective = cands[*pick].id;
			const auto *o = this->find_enemy(world, *this->objective);
			log::log(INFO << "AI P" << this->player << ": attack wave " << this->waves << " ("
			              << this->wave.size() << " units) next objective " << o->name << " " << o->id
			              << " (P" << o->owner << ") at (" << fmt1(o->ne) << ", " << fmt1(o->se)
			              << "), t=" << fmt1(now) << " s");
		}
		const auto *objective = this->find_enemy(world, *this->objective);
		for (const auto &unit : world.army) {
			if (not this->wave.contains(unit.id)) {
				continue;
			}
			auto target = combat->get_target(unit.id);
			const Seen *current = target ? this->find_enemy(world, *target) : nullptr;
			// attack move: enemy units in sight first, buildings are the objective's turn
			auto cands = this->candidates(world, unit.ne, unit.se);
			auto pick = choose_unit_target(cands, unit.sight + this->params.retarget_extra);
			if (pick) {
				if (current == nullptr or current->building) {
					this->order_attack(state, unit, *this->find_enemy(world, cands[*pick].id), time);
				}
				continue;
			}
			if (current == nullptr) {
				// XR fork W4: march with an attack move, buildings (no automatic targets) when close
				if (objective->building and dist(unit.ne, unit.se, objective->ne, objective->se) - objective->radius
				                                  > unit.sight + 1.0) {
					if (combat->get_order(unit.id) != combat::order_t::ATTACK_MOVE) {
						this->order_attack_move(state, unit, objective->ne, objective->se, time);
					}
				}
				else {
					this->order_attack(state, unit, *objective, time);
				}
			}
		}
		return;
	}

	if (this->defending) {
		return;
	}

	// army at home: start a wave?
	std::vector<const Seen *> home;
	for (const auto &unit : world.army) {
		if (not combat->get_target(unit.id)) {
			home.push_back(&unit);
		}
	}
	auto decision = attack_decision(now, home.size(), this->waves, this->last_wave, this->params);
	if (not decision.attack) {
		std::ostringstream wait;
		wait << to_string(decision.reason);
		if (wait.str() != this->last_attack_wait) {
			this->last_attack_wait = wait.str();
			if (decision.reason != attack_reason_t::NO_ARMY or this->waves > 0) {
				log::log(INFO << "AI P" << this->player << ": no attack yet (" << wait.str() << ", army "
				              << home.size() << "/" << this->params.attack_threshold << ", next at t>="
				              << fmt1(next_attack_time(now, home.size(), this->waves, this->last_wave, this->params))
				              << " s), t=" << fmt1(now) << " s");
			}
		}
		return;
	}

	double rne = world.has_home ? world.home_ne : home.front()->ne;
	double rse = world.has_home ? world.home_se : home.front()->se;
	auto cands = this->candidates(world, rne, rse);
	auto pick = choose_objective(cands);
	if (not pick) {
		return;
	}
	this->objective = cands[*pick].id;
	const auto *objective = this->find_enemy(world, *this->objective);
	this->waves += 1;
	this->last_wave = now;
	this->last_attack_wait.clear();
	{
		std::lock_guard<std::mutex> lock{this->status_mutex};
		if (not this->status.first_attack) {
			this->status.first_attack = now;
		}
	}
	log::log(INFO << "AI P" << this->player << ": attack wave " << this->waves << " with " << home.size()
	              << " units (" << to_string(decision.reason) << ") -> " << objective->name << " " << objective->id
	              << " (P" << objective->owner << ") at (" << fmt1(objective->ne) << ", " << fmt1(objective->se)
	              << "), t=" << fmt1(now) << " s");
	for (const auto *unit : home) {
		this->wave.insert(unit->id);
	}
	// XR fork W4: the wave marches with an attack move (aggressive) and fights on the way
	this->set_stances(state, world);
	for (const auto *unit : home) {
		if (objective->building) {
			this->order_attack_move(state, *unit, objective->ne, objective->se, time);
		}
		else {
			this->order_attack(state, *unit, *objective, time);
		}
	}
}

// ---------------------------------------------------------------- economy

void AiPlayer::gather(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	const double now = time.to_double();
	auto combat = state->get_combat();

	std::array<size_t, GATHER_KINDS> workers{};
	std::unordered_map<entity_id_t, size_t> assigned;
	std::vector<const Seen *> idle;
	// XR fork W4: enemy buildings that shoot (town centres have a base arrow, towers) – no gathering in their range
	struct Danger {
		double ne, se, reach;
	};
	std::vector<Danger> danger;
	for (const auto &e : world.enemies) {
		if (not e.building) {
			continue;
		}
		if (auto stats = combat->get_stats(e.id); stats and stats->shoots_arrows() and stats->can_attack) {
			danger.push_back({e.ne, e.se, e.radius + stats->max_range + 1.5});
		}
	}
	auto in_danger = [&](double ne, double se) {
		for (const auto &d : danger) {
			if (dist(d.ne, d.se, ne, se) < d.reach) {
				return true;
			}
		}
		return false;
	};

	for (const auto &v : world.villagers) {
		auto entity = state->get_game_entity(v.id);
		if (not entity->has_component(component::component_t::GATHER)) {
			continue;
		}
		auto gather = std::dynamic_pointer_cast<component::Gather>(
			entity->get_component(component::component_t::GATHER));
		const auto &job = gather->get_job();
		if (combat->is_fleeing(v.id)) {
			continue;
		}
		// XR fork W4: a job in the range of an enemy town centre or tower: choose another resource
		if (job.phase != component::Gather::phase_t::NONE and job.target
		    and in_danger(job.target_pos.ne.to_double(), job.target_pos.se.to_double())) {
			idle.push_back(&v);
			continue;
		}
		if (job.phase != component::Gather::phase_t::NONE) {
			auto kind = gather_kind(job.resource);
			if (kind) {
				workers[static_cast<size_t>(*kind)] += 1;
			}
			if (job.target) {
				assigned[*job.target] += 1;
			}
			continue;
		}
		if (combat->get_target(v.id)) {
			continue;
		}
		// builders (production) are busy too
		if (entity->has_component(component::component_t::BUILDER)) {
			auto builder = std::dynamic_pointer_cast<component::Builder>(
				entity->get_component(component::component_t::BUILDER));
			if (builder->get_job().phase != component::Builder::phase_t::NONE) {
				continue;
			}
		}
		// idle again soon after the last order (no path to resource or drop site): wait longer each time
		auto last = this->last_order.find(v.id);
		auto &fails = this->quick_idle[v.id];
		double cooldown = this->params.order_cooldown * static_cast<double>(1 + std::min<size_t>(fails, 10));
		if (last != this->last_order.end() and now - last->second < cooldown) {
			continue;
		}
		if (last != this->last_order.end() and now - last->second < cooldown + 15.0) {
			fails += 1;
			// XR fork W3: idle again soon after the order: its resource is not reachable, leave it for a while
			if (auto spot = this->last_spot.find(v.id); spot != this->last_spot.end()) {
				this->bad_spots[spot->second] = now;
			}
		}
		else {
			fails = 0;
		}
		idle.push_back(&v);
	}
	if (idle.empty()) {
		return;
	}

	// resources of gaia per kind
	struct Spot {
		entity_id_t id;
		double ne;
		double se;
		/// XR fork W3: a farm (one worker)
		bool field = false;
	};
	std::array<std::vector<Spot>, GATHER_KINDS> spots;
	// XR fork W3: one idle villager decides what villagers can gather (hunting, herding, farming)
	const auto sample = state->get_game_entity(idle.front()->id);
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (not entity->has_component(component::component_t::HARVESTABLE)
		    or not entity->has_component(component::component_t::OWNERSHIP)) {
			continue;
		}
		auto harvestable = std::dynamic_pointer_cast<component::Harvestable>(
			entity->get_component(component::component_t::HARVESTABLE));
		auto kind = gather_kind(harvestable->get_resource());
		if (not kind or harvestable->is_depleted() or harvestable->get_amount() <= 0.0) {
			continue;
		}
		auto ownership = std::dynamic_pointer_cast<component::Ownership>(
			entity->get_component(component::component_t::OWNERSHIP));
		const auto owner = ownership->get_owners().get(time);
		const auto source = harvestable->kind;
		// XR fork W3: no boars (they fight back), fish and traps belong to ships; own free farms and
		// own herdables count besides the resources of gaia
		if (source == food::source_t::PREDATOR or source == food::source_t::FISH or source == food::source_t::TRAP) {
			continue;
		}
		if (food::is_field(source)) {
			if (owner != this->player or harvestable->worker) {
				continue;
			}
		}
		else if (source == food::source_t::HERDABLE) {
			if (owner != this->player and not this->neutral.contains(owner)) {
				continue;
			}
		}
		else if (not this->neutral.contains(owner)) {
			continue;
		}
		if (auto bad = this->bad_spots.find(id); bad != this->bad_spots.end() and now - bad->second < 120.0) {
			continue;
		}
		if (source != food::source_t::OTHER and not econ::can_gather_from(sample, entity)) {
			continue;
		}
		auto position = std::dynamic_pointer_cast<component::Position>(
			entity->get_component(component::component_t::POSITION));
		auto pos = position->get_positions().get(time);
		if (in_danger(pos.ne.to_double(), pos.se.to_double())) {
			continue;
		}
		spots[static_cast<size_t>(*kind)].push_back({id, pos.ne.to_double(), pos.se.to_double(), food::is_field(source)});
	}
	std::array<bool, GATHER_KINDS> available{};
	for (size_t k = 0; k < GATHER_KINDS; ++k) {
		std::sort(spots[k].begin(), spots[k].end(), [](const Spot &a, const Spot &b) { return a.id < b.id; });
		available[k] = not spots[k].empty();
	}

	std::array<size_t, GATHER_KINDS> ordered{};
	size_t no_resource = 0;
	for (const auto *v : idle) {
		auto kind = choose_gather(workers, this->params.gather_split, available);
		if (not kind) {
			no_resource += 1;
			continue;
		}
		const auto &list = spots[static_cast<size_t>(*kind)];
		const Spot *best = nullptr;
		double best_score = 1e30;
		for (const auto &spot : list) {
			if (spot.field and assigned[spot.id] > 0) {
				continue;
			}
			// near the villager and the base, spread over the spots
			double score = dist(v->ne, v->se, spot.ne, spot.se)
			               + (world.has_home ? 0.5 * dist(world.home_ne, world.home_se, spot.ne, spot.se) : 0.0)
			               + 3.0 * static_cast<double>(assigned[spot.id]);
			if (score < best_score) {
				best_score = score;
				best = &spot;
			}
		}
		if (best == nullptr) {
			continue;
		}
		auto entity = state->get_game_entity(v->id);
		combat->cancel_attack(v->id, time);
		auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
			entity->get_component(component::component_t::COMMANDQUEUE));
		queue->add_command(time, std::make_shared<component::command::GatherCommand>(best->id));
		this->last_order[v->id] = now;
		this->last_spot[v->id] = best->id;
		workers[static_cast<size_t>(*kind)] += 1;
		ordered[static_cast<size_t>(*kind)] += 1;
		assigned[best->id] += 1;
		std::lock_guard<std::mutex> lock{this->status_mutex};
		this->status.gather_orders += 1;
	}

	size_t total = ordered[0] + ordered[1] + ordered[2];
	if ((total > 0 or no_resource > 0) and this->gather_log.due(now)) {
		auto suppressed = this->gather_log.take_suppressed();
		log::log(INFO << "AI P" << this->player << ": " << total << " idle villagers gather (food " << ordered[0]
		              << ", wood " << ordered[1] << ", gold " << ordered[2] << "), workers food " << workers[0]
		              << ", wood " << workers[1] << ", gold " << workers[2]
		              << (no_resource > 0 ? ", " + std::to_string(no_resource) + " without resources" : std::string{})
		              << (suppressed > 0 ? " (+" + std::to_string(suppressed) + " rounds not logged)" : std::string{})
		              << ", t=" << fmt1(now) << " s");
	}
}

// ---------------------------------------------------------------- production

std::optional<std::pair<double, double>> AiPlayer::building_spot(const std::shared_ptr<GameState> &state,
                                                                 const World &world,
                                                                 const std::string &name,
                                                                 double side,
                                                                 const time::time_t &time) {
	if (not world.has_home) {
		return std::nullopt;
	}
	auto map = state->get_map();
	if (this->land_grid == -2) {
		this->land_grid = -1;
		auto mods = state->get_mod_manager();
		if (mods) {
			for (const auto &modpack : mods->get_load_order()) {
				try {
					this->land_grid = static_cast<long>(map->get_grid_id(modpack + ".data.util.path_type.types.Land"));
					break;
				}
				catch (std::exception &) {
				}
			}
		}
	}
	auto size = map->get_size();
	// mines and bushes near home: villagers walk between them and the town center
	std::vector<std::pair<double, double>> keep_clear;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (not entity->has_component(component::component_t::HARVESTABLE)) {
			continue;
		}
		auto harvestable = std::dynamic_pointer_cast<component::Harvestable>(
			entity->get_component(component::component_t::HARVESTABLE));
		if (harvestable->get_resource() == resource_t::WOOD or harvestable->is_depleted()) {
			continue;
		}
		auto position = std::dynamic_pointer_cast<component::Position>(
			entity->get_component(component::component_t::POSITION));
		auto pos = position->get_positions().get(time);
		if (dist(pos.ne.to_double(), pos.se.to_double(), world.home_ne, world.home_se) < 16.0) {
			keep_clear.emplace_back(pos.ne.to_double(), pos.se.to_double());
		}
	}
	std::sort(keep_clear.begin(), keep_clear.end());
	auto spots = building_spots(world.home_ne, world.home_se, 5.0, 10.0, side, this->rng);
	for (const auto &[x, y] : spots) {
		bool ok = true;
		long t0x = static_cast<long>(std::floor(x - side / 2.0 + 0.01));
		long t0y = static_cast<long>(std::floor(y - side / 2.0 + 0.01));
		long n = static_cast<long>(std::lround(side));
		// the footprint and a ring of one tile around it must be free (no enclosed villagers)
		for (long ty = t0y - 1; ok and ty < t0y + n + 1; ++ty) {
			for (long tx = t0x - 1; ok and tx < t0x + n + 1; ++tx) {
				if (tx < 0 or ty < 0 or tx >= static_cast<long>(size[0]) or ty >= static_cast<long>(size[1])) {
					ok = false;
					continue;
				}
				coord::tile tile{static_cast<coord::tile_t>(tx), static_cast<coord::tile_t>(ty)};
				if (this->land_grid >= 0) {
					ok = map->is_passable(static_cast<path::grid_id_t>(this->land_grid), tile);
				}
				// landscapes: no foundations on ice or fords (same rule as the production check)
				bool inside = tx >= t0x and tx < t0x + n and ty >= t0y and ty < t0y + n;
				if (ok and inside) {
					ok = prod::terrain_buildable(map->terrain_name(tile));
				}
			}
		}
		// at least 3 tiles from mines and bushes
		for (size_t i = 0; ok and i < keep_clear.size(); ++i) {
			ok = dist(x, y, keep_clear[i].first, keep_clear[i].second) >= side / 2.0 + 3.0;
		}
		// keep a lane free around own buildings
		for (size_t i = 0; ok and i < world.buildings.size(); ++i) {
			const auto &b = world.buildings[i];
			ok = dist(x, y, b.ne, b.se) >= b.radius + side / 2.0 + 1.0;
		}
		// last and most expensive: the rules of the production itself (elevation, explored, units)
		if (ok and not world.villagers.empty()) {
			coord::phys3 where{coord::phys_t{x}, coord::phys_t{y}, coord::phys_t{0}};
			ok = this->production->can_place(state, name, world.villagers.front().id, where, time);
		}
		if (ok) {
			return std::pair{x, y};
		}
	}
	return std::nullopt;
}

std::vector<const AiPlayer::Seen *> AiPlayer::free_builders(const std::shared_ptr<GameState> &state,
                                                            const World &world,
                                                            double ne,
                                                            double se,
                                                            const time::time_t &time) const {
	const double now = time.to_double();
	auto combat = state->get_combat();
	std::vector<const Seen *> result;
	for (const auto &v : world.villagers) {
		auto last = this->last_order.find(v.id);
		if (last != this->last_order.end() and now - last->second < 2.0 * this->params.order_cooldown) {
			continue;
		}
		if (combat->get_target(v.id)) {
			continue;
		}
		auto entity = state->get_game_entity(v.id);
		if (not entity->has_component(component::component_t::BUILDER)) {
			continue;
		}
		auto builder = std::dynamic_pointer_cast<component::Builder>(
			entity->get_component(component::component_t::BUILDER));
		if (builder->get_job().phase != component::Builder::phase_t::NONE) {
			continue;
		}
		// walking units cannot take a build job reliably (no path from a moving position)
		if (entity->has_component(component::component_t::GATHER)) {
			auto gather = std::dynamic_pointer_cast<component::Gather>(
				entity->get_component(component::component_t::GATHER));
			auto phase = gather->get_job().phase;
			if (phase != component::Gather::phase_t::NONE and phase != component::Gather::phase_t::GATHERING) {
				continue;
			}
		}
		result.push_back(&v);
	}
	std::stable_sort(result.begin(), result.end(), [&](const Seen *a, const Seen *b) {
		return dist(a->ne, a->se, ne, se) < dist(b->ne, b->se, ne, se);
	});
	return result;
}

void AiPlayer::resume_foundations(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	if (not this->production->enabled()) {
		return;
	}
	const double now = time.to_double();
	// foundations someone works on
	std::unordered_set<entity_id_t> worked;
	for (const auto &v : world.villagers) {
		auto entity = state->get_game_entity(v.id);
		if (entity->has_component(component::component_t::BUILDER)) {
			auto builder = std::dynamic_pointer_cast<component::Builder>(
				entity->get_component(component::component_t::BUILDER));
			const auto &job = builder->get_job();
			if (job.phase != component::Builder::phase_t::NONE and job.target) {
				worked.insert(*job.target);
			}
		}
	}
	for (const auto &b : world.buildings) {
		if (worked.contains(b.id) or this->production->is_complete(state, b.id, time)) {
			continue;
		}
		// 10 s between assignments, longer after repeated failures (at most 60 s)
		auto last = this->last_resume.find(b.id);
		if (last != this->last_resume.end()
		    and now - last->second.first < std::min(60.0, 10.0 * static_cast<double>(last->second.second))) {
			continue;
		}
		auto candidates = this->free_builders(state, world, b.ne, b.se, time);
		if (candidates.empty()) {
			continue;
		}
		const auto *v = candidates.front();
		state->get_combat()->cancel_attack(v->id, time);
		auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
			state->get_game_entity(v->id)->get_component(component::component_t::COMMANDQUEUE));
		queue->add_command(time, std::make_shared<component::command::BuildCommand>(b.id));
		this->last_order[v->id] = now;
		auto &resume = this->last_resume[b.id];
		resume.first = now;
		resume.second += 1;
		log::log(INFO << "AI P" << this->player << ": villager " << v->id << " builds " << b.name << " "
		              << b.id << " at (" << fmt1(b.ne) << ", " << fmt1(b.se) << "), assignment " << resume.second
		              << ", t=" << fmt1(now) << " s");
	}
	std::erase_if(this->last_resume, [&](const auto &entry) { return not this->last_health.contains(entry.first); });
}

void AiPlayer::produce(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	const double now = time.to_double();
	this->resume_foundations(state, world, time);
	EconomyView view;
	std::vector<const Seen *> town_centers, barracks;
	for (const auto &b : world.buildings) {
		bool complete = this->production->is_complete(state, b.id, time);
		if (b.town_center) {
			if (complete) {
				view.town_centers += 1;
				town_centers.push_back(&b);
				view.queued_villagers += this->production->queued(state, b.id, time);
			}
		}
		else if (b.name == "House") {
			(complete ? view.houses : view.houses_planned) += 1;
		}
		else if (b.name == "Barracks") {
			if (complete) {
				view.barracks += 1;
				barracks.push_back(&b);
				view.queued_military += this->production->queued(state, b.id, time);
			}
			else {
				view.barracks_planned += 1;
			}
		}
	}
	view.villagers = world.villagers.size();
	view.military = world.army.size();
	auto population = this->production->population(state, this->player, time);
	if (population) {
		view.population = population->used;
		view.population_cap = population->cap;
	}
	else {
		view.population = view.villagers + view.military;
		view.population_cap = estimated_population_cap(view.town_centers, view.houses);
	}
	auto stock = state->get_player(this->player)->get_resources().get();
	for (size_t i = 0; i < view.stock.size(); ++i) {
		view.stock[i] = stock[i];
	}

	// XR fork W1: the AI saves its food for the Feudal Age and then the Wheelbarrow (no farms or hunting yet,
	// W3): no villagers or militia until the research is queued
	const auto *age_state = state->get_tech() ? state->get_tech()->find(this->player) : nullptr;
	const int own_age = age_state ? age_state->age : tech::DARK_AGE;
	bool wheelbarrow_done = false;
	if (age_state) {
		for (const auto &t : age_state->facts.researched) {
			wheelbarrow_done = wheelbarrow_done or tech::short_of(t) == "WheelBarrow";
		}
	}
	const tech::AiResearchParams research_params{};
	const bool saving_food = this->production->enabled() and not town_centers.empty()
	                         and not this->production->researching(state, this->player, time)
	                         and ((own_age == tech::DARK_AGE and view.villagers >= research_params.feudal_villagers)
	                              or (own_age >= 2 and not wheelbarrow_done
	                                  and view.villagers >= research_params.wheelbarrow_villagers));
	auto plan = plan_production(view, this->params);
	std::ostringstream text;
	for (auto wish : plan) {
		bool done = false;
		std::string detail;
		switch (wish) {
		case plan_t::TRAIN_VILLAGER:
		case plan_t::TRAIN_MILITIA: {
			if (saving_food) {
				detail = " (saving food for research)";
				break;
			}
			bool villager = wish == plan_t::TRAIN_VILLAGER;
			const auto &where = villager ? town_centers : barracks;
			if (where.empty()) {
				continue;
			}
			done = this->production->train(state, this->player, where.front()->id,
			                               villager ? "Villager" : "Militia", time);
			detail = " in " + where.front()->name + " " + std::to_string(where.front()->id);
		} break;
		case plan_t::BUILD_HOUSE:
		case plan_t::BUILD_BARRACKS: {
			bool house = wish == plan_t::BUILD_HOUSE;
			std::string name = house ? "House" : "Barracks";
			// one placement per building kind every 15 s (rejected spots are retried later)
			auto last = this->last_build.find(name);
			if (last != this->last_build.end() and now - last->second < 15.0) {
				detail = " (waiting for the last placement)";
				break;
			}
			this->last_build[name] = now;
			double side = house ? 2.0 : 3.0;
			auto spot = this->building_spot(state, world, name, side, time);
			if (not spot) {
				detail = " (no free spot)";
				break;
			}
			// nearest free villagers build (one for a house, two for a barracks)
			auto by_distance = this->free_builders(state, world, spot->first, spot->second, time);
			if (by_distance.empty()) {
				// not placed: retry with the next decision
				this->last_build.erase(name);
				detail = " (no free villager)";
				break;
			}
			std::vector<entity_id_t> builders;
			for (size_t i = 0; i < by_distance.size() and i < (house ? 1u : 2u); ++i) {
				builders.push_back(by_distance[i]->id);
			}
			coord::phys3 where{coord::phys_t{spot->first}, coord::phys_t{spot->second}, coord::phys_t{0}};
			done = this->production->build(state, this->player, name, builders, where, time);
			for (auto id : builders) {
				// builders are not sent gathering before their build command arrives
				this->last_order[id] = now;
			}
			detail = " at (" + fmt1(spot->first) + ", " + fmt1(spot->second) + ") by " + std::to_string(builders.size())
			         + " villager" + (builders.size() == 1 ? "" : "s");
		} break;
		default:
			break;
		}
		{
			std::lock_guard<std::mutex> lock{this->status_mutex};
			this->status.production_requests += 1;
			this->status.production_done += done ? 1 : 0;
		}
		text << (text.tellp() > 0 ? "; " : "") << to_string(wish) << detail << (done ? "" : " [not done]");
	}

	// XR fork W1: research in the first town centre (Loom, Feudal Age, Wheelbarrow line; tech_math.h)
	if (not town_centers.empty() and this->production->enabled() and now - this->last_research >= 10.0) {
		tech::AiResearchView rv;
		rv.villagers = view.villagers;
		rv.stock = stock;
		const auto *own_tech = state->get_tech() ? state->get_tech()->find(this->player) : nullptr;
		rv.age = own_tech ? own_tech->age : tech::DARK_AGE;
		rv.researching = this->production->researching(state, this->player, time);
		rv.offered = this->production->offered_research(state, this->player, town_centers.front()->id, time);
		auto choice = tech::ai_research_choice(rv);
		// the Feudal Age needs two buildings of the Dark Age (data: 2 of mill, lumber camp, dock, barracks,
		// mining camp); besides its barracks the AI builds a lumber camp
		const bool feudal_locked = rv.age == tech::DARK_AGE
		                           and rv.villagers >= tech::AiResearchParams{}.feudal_villagers
		                           and std::find(rv.offered.begin(), rv.offered.end(), "FeudalAge") == rv.offered.end();
		// XR fork W3: the mill comes early now; a lumber camp is the second building unless there is one
		bool has_camp = false;
		for (const auto &b : world.buildings) {
			has_camp = has_camp or b.name == "LumberCamp" or b.name == "MiningCamp" or b.name == "Barracks"
			           or b.name == "Dock";
		}
		auto last_camp = this->last_build.find("LumberCamp");
		if (feudal_locked and not has_camp and stock[static_cast<size_t>(resource_t::WOOD)] >= 100.0
		    and (last_camp == this->last_build.end() or now - last_camp->second >= 15.0)) {
			this->last_build["LumberCamp"] = now;
			auto spot = this->building_spot(state, world, "LumberCamp", 2.0, time);
			std::vector<const Seen *> by_distance;
			if (spot) {
				by_distance = this->free_builders(state, world, spot->first, spot->second, time);
			}
			if (spot and not by_distance.empty()) {
				coord::phys3 where{coord::phys_t{spot->first}, coord::phys_t{spot->second}, coord::phys_t{0}};
				const bool placed = this->production->build(state, this->player, "LumberCamp", {by_distance.front()->id},
				                                            where, time);
				this->last_order[by_distance.front()->id] = now;
				log::log(INFO << "AI P" << this->player << " builds a lumber camp for the Feudal Age at ("
				              << fmt1(spot->first) << ", " << fmt1(spot->second) << ")" << (placed ? "" : " [not done]"));
			}
		}
		if (not choice.empty()) {
			this->last_research = now;
			const bool done = this->production->research(state, this->player, town_centers.front()->id, choice, time);
			log::log(INFO << "AI P" << this->player << " research " << choice << " in town centre "
			              << town_centers.front()->id << (done ? "" : " [not done]") << ", villagers "
			              << view.villagers << ", age " << rv.age << ", t=" << fmt1(now) << " s");
		}
	}

	// log when the wishes change, else once per report period
	std::ostringstream key;
	for (auto wish : plan) {
		key << static_cast<int>(wish) << ",";
	}
	bool changed = key.str() != this->last_plan;
	if (not plan.empty() and (changed ? (this->plan_log.due(now), true) : this->plan_log.due(now))) {
		log::log(INFO << "AI P" << this->player << " production (" << this->production->name()
		              << (this->production->enabled() ? "" : ", no effect yet") << "): " << text.str()
		              << " | population " << view.population << "/" << view.population_cap << ", villagers "
		              << view.villagers << "/" << this->params.villager_target << ", t=" << fmt1(now) << " s");
	}
	this->last_plan = key.str();
}

// ---------------------------------------------------------------- food (XR fork W3)

void AiPlayer::food_economy(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	const double now = time.to_double();
	auto combat = state->get_combat();
	auto food_state = state->get_food();
	if (food_state == nullptr or not world.has_home) {
		return;
	}

	// 1. herding: own sheep walk to the town centre (spec 02 §2.4), wild food near home counts
	bool has_tc = false;
	for (const auto &b : world.buildings) {
		has_tc = has_tc or (b.town_center and this->production->is_complete(state, b.id, time));
	}
	long land_grid = -1;
	if (auto mods = state->get_mod_manager()) {
		for (const auto &modpack : mods->get_load_order()) {
			try {
				land_grid = static_cast<long>(state->get_map()->get_grid_id(modpack + ".data.util.path_type.types.Land"));
				break;
			}
			catch (std::exception &) {
			}
		}
	}
	size_t natural = 0;
	size_t herded = 0;
	for (const auto &[id, entity] : state->get_game_entities()) {
		auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
		if (h == nullptr or h->is_depleted() or h->get_resource() != resource_t::FOOD) {
			continue;
		}
		const auto pos = std::dynamic_pointer_cast<component::Position>(
			entity->get_component(component::component_t::POSITION))->get_positions().get(time);
		const double d = dist(pos.ne.to_double(), pos.se.to_double(), world.home_ne, world.home_se);
		const auto owner = std::dynamic_pointer_cast<component::Ownership>(
			entity->get_component(component::component_t::OWNERSHIP))->get_owners().get(time);
		const bool wild = (h->kind == food::source_t::OTHER or h->kind == food::source_t::PREY
		                   or h->kind == food::source_t::HERDABLE)
		                  and (this->neutral.contains(owner) or owner == this->player);
		auto bad = this->bad_spots.find(id);
		if (wild and d <= FOOD_NEAR and not(bad != this->bad_spots.end() and now - bad->second < 120.0)) {
			natural += 1;
		}
		if (h->kind != food::source_t::HERDABLE or h->dead or owner != this->player or combat->is_dead(id) or d <= 4.0) {
			continue;
		}
		auto last = this->last_herd.find(id);
		if (last != this->last_herd.end() and now - last->second < 8.0) {
			continue;
		}
		auto queue = component_of<component::CommandQueue>(entity, component::component_t::COMMANDQUEUE);
		if (queue == nullptr) {
			continue;
		}
		// a spot 3 tiles from the town centre towards the animal
		const double dn = pos.ne.to_double() - world.home_ne;
		const double ds = pos.se.to_double() - world.home_se;
		const double len = std::max(1e-6, std::hypot(dn, ds));
		const auto msize = state->get_map()->get_size();
		const double hne = std::clamp(world.home_ne + dn / len * 3.0, 0.5, static_cast<double>(msize[0]) - 0.5);
		const double hse = std::clamp(world.home_se + ds / len * 3.0, 0.5, static_cast<double>(msize[1]) - 0.5);
		// only to a free land tile next to a standing town centre
		if (not has_tc or land_grid < 0
		    or not state->get_map()->is_passable(static_cast<path::grid_id_t>(land_grid),
		                                         coord::tile{static_cast<coord::tile_t>(std::floor(hne)),
		                                                     static_cast<coord::tile_t>(std::floor(hse))})) {
			continue;
		}
		auto dest = state->get_map()->on_terrain(coord::phys3{coord::phys_t{hne}, coord::phys_t{hse}, coord::phys_t{0.0}});
		queue->add_command(time, std::make_shared<component::command::MoveCommand>(dest));
		this->last_herd[id] = now;
		herded += 1;
	}
	if (herded > 0 and this->gather_log.due(now)) {
		log::log(INFO << "AI P" << this->player << ": herds " << herded << " sheep to the town centre, t=" << fmt1(now)
		              << " s");
	}

	// 2. mill, farms, reseeds (ai_rules.h plan_food)
	if (not this->production->enabled()) {
		return;
	}
	FoodView view;
	view.villagers = world.villagers.size();
	view.natural_food = natural;
	view.reseeds = food_state->reseeds(this->player);
	for (const auto &b : world.buildings) {
		const bool complete = this->production->is_complete(state, b.id, time);
		if (b.name == "Mill") {
			(complete ? view.mills : view.mills_planned) += 1;
		}
		else if (b.name == "Farm" and not food_state->is_decayed(b.id)) {
			(complete ? view.farms : view.farms_planned) += 1;
		}
	}
	auto stock = state->get_player(this->player)->get_resources().get();
	for (size_t i = 0; i < view.stock.size(); ++i) {
		view.stock[i] = stock[i];
	}
	auto plan = plan_food(view);
	auto place = [&](const std::string &name, double side) {
		auto last = this->last_build.find(name);
		if (last != this->last_build.end() and now - last->second < 15.0) {
			return;
		}
		this->last_build[name] = now;
		auto spot = this->building_spot(state, world, name, side, time);
		if (not spot) {
			log::log(INFO << "AI P" << this->player << ": no spot for a " << name << ", t=" << fmt1(now) << " s");
			return;
		}
		auto builders = this->free_builders(state, world, spot->first, spot->second, time);
		if (builders.empty()) {
			this->last_build.erase(name);
			return;
		}
		coord::phys3 where{coord::phys_t{spot->first}, coord::phys_t{spot->second}, coord::phys_t{0}};
		const bool done = this->production->build(state, this->player, name, {builders.front()->id}, where, time);
		this->last_order[builders.front()->id] = now;
		log::log(INFO << "AI P" << this->player << " builds a " << name << " at (" << fmt1(spot->first) << ", "
		              << fmt1(spot->second) << ") by villager " << builders.front()->id << (done ? "" : " [not done]")
		              << " (mills " << view.mills << ", farms " << view.farms << "+" << view.farms_planned << "/"
		              << plan.farm_target << ", wild food near home " << natural << "), t=" << fmt1(now) << " s");
	};
	if (plan.build_mill) {
		place("Mill", 2.0);
	}
	if (plan.build_farm) {
		place("Farm", 3.0);
	}
	if (plan.reseed and now - this->last_reseed >= 10.0) {
		this->last_reseed = now;
		const bool done = this->production->reseed(state, this->player, time);
		log::log(INFO << "AI P" << this->player << " queues a farm reseed (" << view.reseeds << " queued, farms "
		              << view.farms << ")" << (done ? "" : " [not done]") << ", t=" << fmt1(now) << " s");
	}
	std::erase_if(this->last_herd, [&](const auto &entry) { return now - entry.second > 60.0; });
	std::erase_if(this->bad_spots, [&](const auto &entry) { return now - entry.second > 120.0; });
}

// ---------------------------------------------------------------- buildings (XR fork W5)

void AiPlayer::fortify(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	if (not this->production->enabled() or not world.has_home or this->defending) {
		return;
	}
	const double now = time.to_double();
	if (now - this->last_palisade < 20.0) {
		return;
	}
	const auto *age_state = state->get_tech() ? state->get_tech()->find(this->player) : nullptr;
	if (age_state == nullptr or age_state->age < 2) {
		return;
	}
	const auto size = static_cast<long>(state->get_map()->get_size()[0]);
	const auto drags = palisade_ring(static_cast<long>(std::floor(world.home_ne)), static_cast<long>(std::floor(world.home_se)),
	                                 PALISADE_RADIUS, PALISADE_GAP, size);
	if (this->palisade_next >= drags.size()) {
		return;
	}
	const auto &d = drags[this->palisade_next];
	const long tiles = std::max(std::abs(d.x1 - d.x0), std::abs(d.y1 - d.y0)) + 1;
	const double wood = state->get_player(this->player)->get_resources().get()[static_cast<size_t>(resource_t::WOOD)];
	// 2 wood per segment in the data; the reserve keeps houses and farms going
	if (wood < PALISADE_WOOD_RESERVE + 2.0 * static_cast<double>(tiles)) {
		return;
	}
	const double mx = 0.5 * static_cast<double>(d.x0 + d.x1) + 0.5;
	const double my = 0.5 * static_cast<double>(d.y0 + d.y1) + 0.5;
	auto builders = this->free_builders(state, world, mx, my, time);
	if (builders.empty()) {
		return;
	}
	this->last_palisade = now;
	auto at = [](long x, long y) {
		return coord::phys3{coord::phys_t{static_cast<double>(x) + 0.5}, coord::phys_t{static_cast<double>(y) + 0.5},
		                    coord::phys_t{0.0}};
	};
	const bool done = this->production->build_wall(state, this->player, "PalisadeWall", {builders.front()->id},
	                                               at(d.x0, d.y0), at(d.x1, d.y1), time);
	this->last_order[builders.front()->id] = now;
	log::log(INFO << "AI P" << this->player << " drags a palisade " << (this->palisade_next + 1) << "/" << drags.size()
	              << " from (" << d.x0 << ", " << d.y0 << ") to (" << d.x1 << ", " << d.y1 << "), " << tiles
	              << " tiles, villager " << builders.front()->id << (done ? "" : " [not done]") << ", t=" << fmt1(now)
	              << " s");
	this->palisade_next += 1;
}

void AiPlayer::repair(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	if (not this->production->enabled() or this->defending) {
		return;
	}
	const double now = time.to_double();
	// buildings someone repairs already
	std::unordered_set<entity_id_t> repaired;
	for (const auto &v : world.villagers) {
		auto entity = state->get_game_entity(v.id);
		if (auto b = component_of<component::Builder>(entity, component::component_t::BUILDER)) {
			const auto &job = b->get_job();
			if (job.repair and job.target) {
				repaired.insert(*job.target);
			}
		}
	}
	for (const auto &b : world.buildings) {
		auto entity = state->get_game_entity(b.id);
		if (repaired.contains(b.id) or not prod::is_damaged(state, entity, time)) {
			continue;
		}
		auto last = this->last_repair.find(b.id);
		if (last != this->last_repair.end() and now - last->second < 15.0) {
			continue;
		}
		// no enemies next to it (the repairer would be killed)
		bool threatened = false;
		for (const auto &e : world.enemies) {
			threatened = threatened or dist(e.ne, e.se, b.ne, b.se) < this->params.defense_radius;
		}
		if (threatened) {
			continue;
		}
		auto candidates = this->free_builders(state, world, b.ne, b.se, time);
		if (candidates.empty()) {
			return;
		}
		const auto *v = candidates.front();
		auto queue = component_of<component::CommandQueue>(state->get_game_entity(v->id), component::component_t::COMMANDQUEUE);
		queue->add_command(time, std::make_shared<component::command::BuildCommand>(b.id));
		this->last_order[v->id] = now;
		this->last_repair[b.id] = now;
		log::log(INFO << "AI P" << this->player << ": villager " << v->id << " repairs " << b.name << " " << b.id
		              << ", t=" << fmt1(now) << " s");
		return;  // one repair order per round
	}
	std::erase_if(this->last_repair, [&](const auto &entry) { return not this->last_health.contains(entry.first); });
}

// ---------------------------------------------------------------- report

// ---------------------------------------------------------------- special (XR fork W6)

namespace {

/// lower case terrain name of a tile
std::string terrain_lower(const std::shared_ptr<Map> &map, long x, long y) {
	auto name = map->terrain_name(coord::tile{static_cast<coord::tile_t>(x), static_cast<coord::tile_t>(y)});
	std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return name;
}

} // namespace

void AiPlayer::special(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	auto special_state = state->get_special();
	auto combat = state->get_combat();
	if (special_state == nullptr or not this->production->enabled()) {
		return;
	}
	const double now = time.to_double();
	auto map = state->get_map();
	if (not this->water) {
		// the map has water (docks, ships): one look at the terrain
		bool any = false;
		const auto size = map->get_size();
		for (size_t y = 0; y < size[1] and not any; y += 2) {
			for (size_t x = 0; x < size[0] and not any; x += 2) {
				const auto name = terrain_lower(map, static_cast<long>(x), static_cast<long>(y));
				any = name.find("water") != std::string::npos and name.find("shallow") == std::string::npos;
			}
		}
		this->water = any;
		log::log(INFO << "AI P" << this->player << " special: map " << (any ? "with" : "without") << " water");
	}

	const auto *own_tech = state->get_tech() ? state->get_tech()->find(this->player) : nullptr;
	SpecialView v;
	v.age = own_tech ? own_tech->age : tech::DARK_AGE;
	v.villagers = world.villagers.size();
	v.water = *this->water;
	const Seen *town_center = nullptr;
	const Seen *monastery = nullptr;
	const Seen *workshop = nullptr;
	const Seen *dock = nullptr;
	for (const auto &b : world.buildings) {
		const bool complete = this->production->is_complete(state, b.id, time);
		if (b.town_center) {
			if (complete and town_center == nullptr) {
				town_center = &b;
			}
		}
		else if (b.name == "Blacksmith" or b.name == "ArcheryRange" or b.name == "Stable" or b.name == "Market") {
			v.feudal_buildings += complete ? 1 : 0;
			v.feudal_planned = v.feudal_planned or not complete;
		}
		else if (b.name == "Monastery") {
			(complete ? v.monastery : v.monastery_planned) = true;
			monastery = complete ? &b : monastery;
		}
		else if (b.name == "SiegeWorkshop") {
			(complete ? v.workshop : v.workshop_planned) = true;
			workshop = complete ? &b : workshop;
		}
		else if (b.name == "Dock") {
			(complete ? v.dock : v.dock_planned) = true;
			dock = complete ? &b : dock;
		}
	}
	v.monks = world.monks.size() + (monastery ? this->production->queued(state, monastery->id, time) : 0);
	for (const auto &u : world.army) {
		v.rams += u.name.find("Ram") != std::string::npos ? 1 : 0;
		v.mangonels += u.name == "Mangonel" or u.name == "Onager" ? 1 : 0;
	}
	if (workshop) {
		// queued siege counts as one of each kind
		const size_t queued = this->production->queued(state, workshop->id, time);
		v.rams += queued > 0 ? 1 : 0;
		v.mangonels += queued > 1 ? 1 : 0;
	}
	v.galleys = world.fleet.size() + (dock ? this->production->queued(state, dock->id, time) : 0);
	if (town_center) {
		auto offered = this->production->offered_research(state, this->player, town_center->id, time);
		v.castle_offered = std::find(offered.begin(), offered.end(), "CastleAge") != offered.end();
		v.researching = this->production->researching(state, this->player, time);
	}

	auto plan = plan_special(v);
	for (auto wish : plan) {
		const int key = static_cast<int>(wish);
		auto last = this->last_special.find(key);
		if (last != this->last_special.end() and now - last->second < 15.0) {
			continue;
		}
		this->last_special[key] = now;
		bool done = false;
		std::string detail;
		auto build = [&](const std::string &name, double side) {
			auto spot = this->building_spot(state, world, name, side, time);
			if (not spot) {
				detail = " (no free spot)";
				return;
			}
			auto builders = this->free_builders(state, world, spot->first, spot->second, time);
			if (builders.empty()) {
				detail = " (no free villager)";
				return;
			}
			coord::phys3 where{coord::phys_t{spot->first}, coord::phys_t{spot->second}, coord::phys_t{0}};
			done = this->production->build(state, this->player, name, {builders.front()->id}, where, time);
			this->last_order[builders.front()->id] = now;
			detail = " " + name + " at (" + fmt1(spot->first) + ", " + fmt1(spot->second) + ")";
		};
		switch (wish) {
		case special_plan_t::BUILD_FEUDAL_BUILDING: {
			bool smith = false;
			for (const auto &b : world.buildings) {
				smith = smith or b.name == "Blacksmith";
			}
			build(smith ? "ArcheryRange" : "Blacksmith", 3.0);
		} break;
		case special_plan_t::RESEARCH_CASTLE:
			if (town_center) {
				done = this->production->research(state, this->player, town_center->id, "CastleAge", time);
			}
			break;
		case special_plan_t::BUILD_MONASTERY:
			build("Monastery", 3.0);
			break;
		case special_plan_t::BUILD_WORKSHOP:
			build("SiegeWorkshop", 4.0);
			break;
		case special_plan_t::TRAIN_MONK:
			done = monastery and this->production->train(state, this->player, monastery->id, "Monk", time);
			break;
		case special_plan_t::TRAIN_RAM:
		case special_plan_t::TRAIN_MANGONEL:
			done = workshop
			       and this->production->train(state, this->player, workshop->id,
			                                   wish == special_plan_t::TRAIN_RAM ? "Ram" : "Mangonel", time);
			break;
		case special_plan_t::TRAIN_GALLEY:
			done = dock and this->production->train(state, this->player, dock->id, "Galley", time);
			break;
		case special_plan_t::BUILD_DOCK: {
			// on water next to the shore, nearest to home (the placement rules of the data decide)
			std::vector<std::pair<double, std::pair<long, long>>> cands;
			const auto size = map->get_size();
			for (long dy = -30; dy <= 30; ++dy) {
				for (long dx = -30; dx <= 30; ++dx) {
					const long x = static_cast<long>(world.home_ne) + dx;
					const long y = static_cast<long>(world.home_se) + dy;
					if (x < 2 or y < 2 or x >= static_cast<long>(size[0]) - 2 or y >= static_cast<long>(size[1]) - 2) {
						continue;
					}
					if (terrain_lower(map, x, y).find("water") == std::string::npos) {
						continue;
					}
					cands.push_back({std::hypot(static_cast<double>(dx), static_cast<double>(dy)), {x, y}});
				}
			}
			std::sort(cands.begin(), cands.end());
			auto builders = this->free_builders(state, world, world.home_ne, world.home_se, time);
			if (builders.empty()) {
				detail = " (no free villager)";
				break;
			}
			// the order is asynchronous: only a spot the production accepts counts as done
			for (size_t i = 0; i < cands.size() and i < 64 and not done; ++i) {
				const auto x = cands[i].second.first;
				const auto y = cands[i].second.second;
				coord::phys3 where{coord::phys_t{static_cast<double>(x) + 0.5}, coord::phys_t{static_cast<double>(y) + 0.5},
				                   coord::phys_t{0}};
				if (not this->production->can_place(state, "Dock", builders.front()->id, where, time)) {
					continue;
				}
				done = this->production->build(state, this->player, "Dock", {builders.front()->id}, where, time);
				if (done) {
					this->last_order[builders.front()->id] = now;
					detail = " at (" + std::to_string(x) + ", " + std::to_string(y) + ")";
				}
			}
		} break;
		}
		log::log(INFO << "AI P" << this->player << " special: " << to_string(wish) << detail << (done ? "" : " [not done]")
		              << ", age " << v.age << ", t=" << fmt1(now) << " s");
	}

	// monks: convert when full and an enemy soldier is near, else stay with the army (healing is automatic)
	double ane = world.home_ne;
	double ase = world.home_se;
	size_t n = 0;
	for (const auto &u : world.army) {
		if (this->wave.empty() or this->wave.contains(u.id)) {
			ane = n == 0 ? u.ne : ane + u.ne;
			ase = n == 0 ? u.se : ase + u.se;
			n += 1;
		}
	}
	if (n > 0) {
		ane /= static_cast<double>(n);
		ase /= static_cast<double>(n);
	}
	for (const auto &m : world.monks) {
		if (special_state->get_job_target(m.id) or special_state->carried_relic(m.id)) {
			continue;
		}
		const double faith = special_state->faith(m.id).value_or(0.0);
		const auto *mt = special_state->type_of(state, state->get_game_entity(m.id), time);
		const double range = mt ? mt->convert_range : 9.0;
		const Seen *target = nullptr;
		double best = 1e18;
		for (const auto &e : world.enemies) {
			const double d = dist(m.ne, m.se, e.ne, e.se);
			if (ai_should_convert(faith, d, range, e.military) and d < best) {
				best = d;
				target = &e;
			}
		}
		if (target) {
			auto reason = special_state->order_convert(state, m.id, target->id, time);
			log::log(INFO << "AI P" << this->player << " special: monk " << m.id << " converts " << target->name << " "
			              << target->id << (reason.empty() ? std::string{} : " [" + reason + "]") << ", t=" << fmt1(now)
			              << " s");
			continue;
		}
		if (n > 0 and dist(m.ne, m.se, ane, ase) > 6.0) {
			auto last = this->last_order.find(m.id);
			if (last != this->last_order.end() and now - last->second < this->params.unit_order_cooldown) {
				continue;
			}
			this->last_order[m.id] = now;
			auto entity = state->get_game_entity(m.id);
			auto queue = component_of<component::CommandQueue>(entity, component::component_t::COMMANDQUEUE);
			if (queue) {
				queue->add_command(time, std::make_shared<component::command::MoveCommand>(map->on_terrain(
											 coord::phys3{coord::phys_t{ane}, coord::phys_t{ase}, coord::phys_t{0}})));
			}
		}
	}

	// rams of a wave attack buildings (buildings are no automatic targets, spec 06 §6.2)
	if (not this->wave.empty()) {
		for (const auto &u : world.army) {
			if (not u.siege or not this->wave.contains(u.id) or u.name.find("Ram") == std::string::npos
			    or combat->get_target(u.id)) {
				continue;
			}
			const Seen *target = nullptr;
			double best = 1e18;
			for (const auto &e : world.enemies) {
				const double d = dist(u.ne, u.se, e.ne, e.se);
				if (e.building and d < best) {
					best = d;
					target = &e;
				}
			}
			if (target and this->order_attack(state, u, *target, time)) {
				log::log(INFO << "AI P" << this->player << " special: ram " << u.id << " attacks " << target->name << " "
				              << target->id << ", t=" << fmt1(now) << " s");
			}
		}
	}
}

void AiPlayer::report(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	const double now = time.to_double();
	if (now < this->next_report) {
		return;
	}
	this->next_report = now + this->params.report_period;

	std::array<size_t, GATHER_KINDS + 1> tasks{};
	for (const auto &v : world.villagers) {
		auto entity = state->get_game_entity(v.id);
		if (not entity->has_component(component::component_t::GATHER)) {
			continue;
		}
		auto gather = std::dynamic_pointer_cast<component::Gather>(
			entity->get_component(component::component_t::GATHER));
		const auto &job = gather->get_job();
		auto kind = gather_kind(job.resource);
		if (job.phase == component::Gather::phase_t::NONE or not kind) {
			tasks[GATHER_KINDS] += 1;
		}
		else {
			tasks[static_cast<size_t>(*kind)] += 1;
		}
	}
	size_t enemy_units = 0, enemy_buildings = 0;
	for (const auto &e : world.enemies) {
		(e.building ? enemy_buildings : enemy_units) += 1;
	}
	size_t home_army = world.army.size() - std::min(world.army.size(), this->wave.size());
	auto stock = state->get_player(this->player)->get_resources().get();
	log::log(INFO << "AI P" << this->player << " report t=" << fmt1(now) << " s: "
	              << (this->defending ? "defend" : (this->wave.empty() ? "economy" : "attack"))
	              << ", food " << std::lround(stock[0]) << " wood " << std::lround(stock[1]) << " gold "
	              << std::lround(stock[2]) << " stone " << std::lround(stock[3]) << ", villagers "
	              << world.villagers.size() << " (food " << tasks[0] << ", wood " << tasks[1] << ", gold "
	              << tasks[2] << ", idle " << tasks[3] << "), army " << world.army.size() << " (wave "
	              << this->wave.size() << "), buildings " << world.buildings.size() << ", enemies "
	              << enemy_units << " units + " << enemy_buildings << " buildings, waves " << this->waves
	              << ", next attack t>="
	              << fmt1(next_attack_time(now, home_army, this->waves, this->last_wave, this->params)) << " s");
}

} // namespace openage::gamestate::ai
