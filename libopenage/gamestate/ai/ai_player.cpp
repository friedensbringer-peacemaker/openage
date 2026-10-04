// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "ai_player.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "event/event_loop.h"
#include "log/log.h"
#include "log/message.h"

#include "assets/mod_manager.h"
#include "coord/tile.h"
#include "gamestate/ai/events.h"
#include "gamestate/ai/production_port.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/gather.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"
#include "gamestate/player.h"


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
	this->gather(state, world, time);
	this->produce(state, world, time);
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
	for (const auto &[id, entity] : state->get_game_entities()) {
		auto stats = combat->get_stats(id);
		if (not stats or combat->is_dead(id) or not stats->attackable()
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

		if (owner != this->player) {
			world.enemies.push_back(std::move(seen));
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
		return;
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
				this->order_attack(state, unit, *objective, time);
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
		this->order_attack(state, *unit, *objective, time);
	}
}

// ---------------------------------------------------------------- economy

void AiPlayer::gather(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	const double now = time.to_double();
	auto combat = state->get_combat();

	std::array<size_t, GATHER_KINDS> workers{};
	std::unordered_map<entity_id_t, size_t> assigned;
	std::vector<const Seen *> idle;
	for (const auto &v : world.villagers) {
		auto entity = state->get_game_entity(v.id);
		if (not entity->has_component(component::component_t::GATHER)) {
			continue;
		}
		auto gather = std::dynamic_pointer_cast<component::Gather>(
			entity->get_component(component::component_t::GATHER));
		const auto &job = gather->get_job();
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
		auto last = this->last_order.find(v.id);
		if (last != this->last_order.end() and now - last->second < this->params.order_cooldown) {
			continue;
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
	};
	std::array<std::vector<Spot>, GATHER_KINDS> spots;
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
		if (not this->neutral.contains(ownership->get_owners().get(time))) {
			continue;
		}
		auto position = std::dynamic_pointer_cast<component::Position>(
			entity->get_component(component::component_t::POSITION));
		auto pos = position->get_positions().get(time);
		spots[static_cast<size_t>(*kind)].push_back({id, pos.ne.to_double(), pos.se.to_double()});
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
                                                                 double side,
                                                                 const time::time_t & /* time */) {
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
	auto spots = building_spots(world.home_ne, world.home_se, 5.0, 10.0, side, this->rng);
	for (const auto &[x, y] : spots) {
		bool ok = true;
		long t0x = static_cast<long>(std::floor(x - side / 2.0 + 0.01));
		long t0y = static_cast<long>(std::floor(y - side / 2.0 + 0.01));
		long n = static_cast<long>(std::lround(side));
		for (long ty = t0y; ok and ty < t0y + n; ++ty) {
			for (long tx = t0x; ok and tx < t0x + n; ++tx) {
				if (tx < 0 or ty < 0 or tx >= static_cast<long>(size[0]) or ty >= static_cast<long>(size[1])) {
					ok = false;
				}
				else if (this->land_grid >= 0) {
					ok = map->is_passable(static_cast<path::grid_id_t>(this->land_grid),
					                      coord::tile{static_cast<coord::tile_t>(tx), static_cast<coord::tile_t>(ty)});
				}
			}
		}
		// keep a lane free around own buildings
		for (size_t i = 0; ok and i < world.buildings.size(); ++i) {
			const auto &b = world.buildings[i];
			ok = dist(x, y, b.ne, b.se) >= b.radius + side / 2.0 + 1.0;
		}
		if (ok) {
			return std::pair{x, y};
		}
	}
	return std::nullopt;
}

void AiPlayer::produce(const std::shared_ptr<GameState> &state, const World &world, const time::time_t &time) {
	const double now = time.to_double();
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

	auto plan = plan_production(view, this->params);
	std::ostringstream text;
	for (auto wish : plan) {
		bool done = false;
		std::string detail;
		switch (wish) {
		case plan_t::TRAIN_VILLAGER:
		case plan_t::TRAIN_MILITIA: {
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
			auto spot = this->building_spot(state, world, side, time);
			if (not spot or world.villagers.empty()) {
				detail = " (no free spot)";
				break;
			}
			// nearest villagers build (one for a house, two for a barracks)
			std::vector<const Seen *> by_distance;
			for (const auto &v : world.villagers) {
				by_distance.push_back(&v);
			}
			std::stable_sort(by_distance.begin(), by_distance.end(), [&](const Seen *a, const Seen *b) {
				return dist(a->ne, a->se, spot->first, spot->second) < dist(b->ne, b->se, spot->first, spot->second);
			});
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

// ---------------------------------------------------------------- report

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
