// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "combat_state.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "error/error.h"
#include "event/event.h"
#include "event/event_loop.h"
#include "log/log.h"
#include "log/message.h"

#include "coord/tile.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/internal/activity.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/move.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/manager.h"
#include "gamestate/map.h"
#include "gamestate/system/idle.h"


namespace openage::gamestate::combat {

namespace {

/// period of the auto attack scan (s)
constexpr double SCAN_PERIOD = 1.0;
/// period of attack ticks while walking towards the target (s)
constexpr double CHASE_PERIOD = 0.4;
/// auto attacks give up targets that are this much beyond the line of sight (tiles)
constexpr double LEASH = 3.0;
/// move orders without any movement before the attacker gives up
constexpr int MAX_CHASE_FAILURES = 4;
/// hit logs per simulation second
constexpr size_t HIT_LOGS_PER_SECOND = 6;

time::time_t seconds(double s) {
	return time::time_t::from_double(s);
}

Point ground(const coord::phys3 &pos) {
	return {pos.ne.to_double(), pos.se.to_double()};
}

} // namespace


CombatState::CombatState(const std::shared_ptr<openage::event::EventLoop> &loop) :
	openage::event::EventEntity{loop},
	loop{loop} {
}

size_t CombatState::id() const {
	// fixed id, the combat state exists once per game
	return 0x636f6d62;
}

std::string CombatState::idstr() const {
	return "combat";
}

// ---------------------------------------------------------------- registration

void CombatState::register_entity(const std::shared_ptr<GameEntity> &entity,
                                  const std::shared_ptr<nyan::View> &db_view,
                                  const nyan::fqon_t &nyan_entity) {
	std::ostringstream key;
	key << nyan_entity << "@" << db_view.get();
	auto cached = this->stats_cache.find(key.str());
	std::shared_ptr<const CombatStats> stats;
	if (cached == this->stats_cache.end()) {
		stats = read_combat_stats(db_view, nyan_entity);
		this->stats_cache.emplace(key.str(), stats);
		log::log(DBG << "Combat stats " << stats->name << ": health " << stats->max_health
		             << ", attack " << attack_sum(stats->attack) << (stats->ranged ? " ranged" : "")
		             << ", reload " << stats->reload_time << " s, range " << stats->max_range
		             << ", sight " << stats->line_of_sight << ", radius " << stats->radius);
	}
	else {
		stats = cached->second;
	}

	if (not stats->alive or not entity->has_component(component::component_t::LIVE)) {
		return;
	}

	auto health = stats->start_health;
	if (stats->building and stats->start_health < stats->max_health) {
		// no construction yet: buildings of the map and of tests stand finished
		auto live = std::dynamic_pointer_cast<component::Live>(
			entity->get_component(component::component_t::LIVE));
		live->set_attribute(time::TIME_ZERO, stats->health_attribute, stats->max_health);
		health = stats->max_health;
	}

	Combatant combatant;
	combatant.entity = entity;
	combatant.stats = stats;
	this->combatants[entity->get_id()] = std::move(combatant);
	this->set_health_snapshot(entity->get_id(), health, stats->max_health, true);
}

void CombatState::start(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	if (this->started) {
		return;
	}
	this->started = true;
	this->loop->create_event("game.combat_scan",
	                         std::static_pointer_cast<CombatState>(state->get_combat()),
	                         state,
	                         time + seconds(SCAN_PERIOD),
	                         openage::event::EventHandler::param_map::map_t{});
}

void CombatState::set_neutral_players(const std::unordered_set<player_id_t> &players) {
	this->neutral = players;
}

// ---------------------------------------------------------------- helpers

CombatState::Combatant *CombatState::find(entity_id_t id) {
	auto it = this->combatants.find(id);
	return it == this->combatants.end() ? nullptr : &it->second;
}

const CombatState::Combatant *CombatState::find(entity_id_t id) const {
	auto it = this->combatants.find(id);
	return it == this->combatants.end() ? nullptr : &it->second;
}

player_id_t CombatState::owner_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) const {
	auto owner = std::dynamic_pointer_cast<component::Ownership>(
		entity->get_component(component::component_t::OWNERSHIP));
	return owner->get_owners().get(time);
}

bool CombatState::is_enemy(const Combatant &a, const Combatant &b, const time::time_t &time) const {
	if (a.entity == b.entity or b.dead or not b.stats->attackable()) {
		return false;
	}
	auto owner_a = this->owner_of(a.entity, time);
	auto owner_b = this->owner_of(b.entity, time);
	return owner_a != owner_b and not this->neutral.contains(owner_a) and not this->neutral.contains(owner_b);
}

coord::phys3 CombatState::position_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) const {
	auto pos = std::dynamic_pointer_cast<component::Position>(
		entity->get_component(component::component_t::POSITION));
	return pos->get_positions().get(time);
}

bool CombatState::is_moving(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) const {
	auto now = ground(this->position_of(entity, time));
	auto soon = ground(this->position_of(entity, time + seconds(0.1)));
	return distance(now, soon) > 1e-3;
}

namespace {

Footprint footprint(const CombatStats &stats, const coord::phys3 &pos) {
	return {ground(pos), stats.radius, stats.building};
}

} // namespace

double CombatState::edge_between(const Combatant &a, const Combatant &b, const time::time_t &time) const {
	return edge_distance(footprint(*a.stats, this->position_of(a.entity, time)),
	                     footprint(*b.stats, this->position_of(b.entity, time)));
}

std::optional<entity_id_t> CombatState::nearest_enemy(const Combatant &self, double range, const time::time_t &time) const {
	std::optional<entity_id_t> best;
	double best_score = 1e30;
	for (const auto &[id, other] : this->combatants) {
		if (not this->is_enemy(self, other, time)) {
			continue;
		}
		double edge = this->edge_between(self, other, time);
		if (edge > range) {
			continue;
		}
		// prefer units that fight back, then other units, then buildings
		double score = edge;
		if (other.stats->building) {
			score += 4.0;
		}
		else if (not other.stats->can_attack or other.stats->villager) {
			score += 2.0;
		}
		if (score < best_score or (score == best_score and best and id < *best)) {
			best_score = score;
			best = id;
		}
	}
	return best;
}

bool CombatState::allow_hit_log(const time::time_t &time) {
	auto second = static_cast<int64_t>(std::floor(time.to_double()));
	if (second != this->log_second) {
		if (this->logs_suppressed > 0) {
			log::log(INFO << "Combat: " << this->logs_suppressed << " more combat messages in second "
			              << this->log_second);
		}
		this->log_second = second;
		this->logs_in_second = 0;
		this->logs_suppressed = 0;
	}
	if (this->logs_in_second < HIT_LOGS_PER_SECOND) {
		this->logs_in_second += 1;
		return true;
	}
	this->logs_suppressed += 1;
	return false;
}

namespace {

std::string describe(const std::shared_ptr<GameEntity> &entity, const CombatStats &stats, player_id_t owner) {
	std::ostringstream out;
	out << stats.name << " " << entity->get_id() << " (P" << owner << ")";
	return out.str();
}

} // namespace

void CombatState::set_health_snapshot(entity_id_t id, int64_t health, int64_t max_health, bool alive) {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	this->health_snapshot[id] = {id, health, max_health, alive};
}

// ---------------------------------------------------------------- orders

bool CombatState::order_attack(const std::shared_ptr<GameState> &state,
                               entity_id_t attacker,
                               entity_id_t target,
                               const time::time_t &time) {
	auto a = this->find(attacker);
	auto t = this->find(target);
	if (not a or not t or a->dead or not a->stats->can_attack or not this->is_enemy(*a, *t, time)) {
		return false;
	}
	if (a->stats->movable) {
		// stop what the unit does (walking, gathering): the attack ticks take over
		this->halt(a->entity, time);
	}
	this->engage(state, *a, target, true, time);
	log::log(INFO << "Combat: " << describe(a->entity, *a->stats, this->owner_of(a->entity, time))
	              << " ordered to attack " << describe(t->entity, *t->stats, this->owner_of(t->entity, time)));
	return true;
}

void CombatState::cancel_attack(entity_id_t attacker, const time::time_t &time) {
	auto a = this->find(attacker);
	if (not a or not a->target) {
		return;
	}
	a->target.reset();
	a->explicit_order = false;
	a->chase_destination.reset();
	if (a->tick) {
		a->tick->cancel(time);
		a->tick = nullptr;
	}
}

std::shared_ptr<GameEntity> CombatState::pick_enemy(const std::shared_ptr<GameState> &state,
                                                    const time::time_t &time,
                                                    player_id_t player,
                                                    const PickRequest &request) const {
	return pick_entity(state, time, request, [&](const std::shared_ptr<GameEntity> &entity) -> std::optional<PickShape> {
		auto c = this->find(entity->get_id());
		if (not c or c->dead or not c->stats->attackable()) {
			return std::nullopt;
		}
		auto owner = this->owner_of(entity, time);
		if (owner == player or this->neutral.contains(owner)) {
			return std::nullopt;
		}
		// units: upright sprite about one tile high; buildings: as high as wide
		return PickShape{c->stats->radius, c->stats->building ? c->stats->radius : 1.0};
	});
}

void CombatState::engage(const std::shared_ptr<GameState> &state,
                         Combatant &attacker,
                         entity_id_t target,
                         bool explicit_order,
                         const time::time_t &time) {
	attacker.target = target;
	attacker.explicit_order = explicit_order;
	attacker.chase_destination.reset();
	attacker.chase_failures = 0;
	this->schedule_tick(state, attacker, time);
}

void CombatState::schedule_tick(const std::shared_ptr<GameState> &state,
                                Combatant &attacker,
                                const time::time_t &time) {
	if (attacker.tick) {
		attacker.tick->cancel(time);
	}
	openage::event::EventHandler::param_map::map_t params{{"entity", attacker.entity->get_id()}};
	attacker.tick = this->loop->create_event("game.combat_tick",
	                                         std::static_pointer_cast<CombatState>(state->get_combat()),
	                                         state,
	                                         time,
	                                         params);
}

void CombatState::drop_target(Combatant &attacker, const time::time_t &time) {
	attacker.target.reset();
	attacker.explicit_order = false;
	attacker.chase_destination.reset();
	if (attacker.tick) {
		attacker.tick->cancel(time);
		attacker.tick = nullptr;
	}
	if (not attacker.dead and not this->is_moving(attacker.entity, time)
	    and attacker.entity->has_component(component::component_t::IDLE)) {
		system::Idle::idle(attacker.entity, time);
	}
}

void CombatState::halt(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto pos = std::dynamic_pointer_cast<component::Position>(
		entity->get_component(component::component_t::POSITION));
	pos->set_position(time, pos->get_positions().get(time));
	// restart the activity: the pending end-of-move event would switch to idle later
	auto activity = std::dynamic_pointer_cast<component::Activity>(
		entity->get_component(component::component_t::ACTIVITY));
	activity->cancel_events(time);
	activity->init(time);
	entity->get_manager()->run_activity_system(time);
}

// ---------------------------------------------------------------- event handlers

void CombatState::tick(const std::shared_ptr<GameState> &state, entity_id_t attacker_id, const time::time_t &time) {
	auto a = this->find(attacker_id);
	if (not a or a->dead) {
		return;
	}
	a->tick = nullptr;
	if (not a->target) {
		return;
	}

	auto t = this->find(*a->target);
	if (not t or not this->is_enemy(*a, *t, time)) {
		// target died or changed sides: continue with the next enemy in sight (not villagers)
		a->target.reset();
		a->explicit_order = false;
		if (not a->stats->villager) {
			double range = a->stats->movable ? std::max(a->stats->line_of_sight, a->stats->max_range)
			                                 : a->stats->max_range;
			auto next = this->nearest_enemy(*a, range, time);
			if (next) {
				this->engage(state, *a, *next, false, time);
				return;
			}
		}
		this->drop_target(*a, time);
		return;
	}

	auto apos = this->position_of(a->entity, time);
	auto tpos = this->position_of(t->entity, time);
	double edge = this->edge_between(*a, *t, time);

	if (in_attack_range(edge, a->stats->min_range, a->stats->max_range)) {
		if (a->stats->movable and this->is_moving(a->entity, time)) {
			this->halt(a->entity, time);
		}
		a->chase_destination.reset();
		a->chase_failures = 0;
		if (a->stats->movable) {
			auto pos = std::dynamic_pointer_cast<component::Position>(
				a->entity->get_component(component::component_t::POSITION));
			auto delta = tpos - apos;
			if (delta.length() > 1e-3) {
				pos->set_angle(time, delta.to_angle());
			}
		}
		if (time >= a->ready_at) {
			a->ready_at = time + seconds(std::max(0.1, a->stats->reload_time));
			if (not a->stats->attack_animation.empty()) {
				a->entity->render_update(time, a->stats->attack_animation);
			}
			this->hit(state, *a, *t, time);
		}
		// the hit may have killed the target; the next tick looks for a new one
		if (not a->dead and a->target) {
			this->schedule_tick(state, *a, std::max(a->ready_at, time + seconds(0.05)));
		}
		return;
	}

	// out of range
	if (not a->stats->movable) {
		this->drop_target(*a, time);
		return;
	}
	double sight = std::max(a->stats->line_of_sight, a->stats->max_range);
	if (not a->explicit_order and edge > sight + LEASH) {
		this->drop_target(*a, time);
		return;
	}

	bool moving = this->is_moving(a->entity, time);
	if (a->chase_destination and not moving and time - a->chase_ordered > seconds(0.5)) {
		// the last move order did not get us there (no path, blocked destination)
		if (edge >= a->last_edge - 0.05) {
			a->chase_failures += 1;
		}
		if (a->chase_failures > MAX_CHASE_FAILURES) {
			log::log(INFO << "Combat: " << describe(a->entity, *a->stats, this->owner_of(a->entity, time))
			              << " cannot reach " << describe(t->entity, *t->stats, this->owner_of(t->entity, time)));
			this->drop_target(*a, time);
			return;
		}
	}
	a->last_edge = edge;

	double gap = a->stats->max_range > 0.0 ? a->stats->max_range * 0.8 : MELEE_RANGE * 0.4;
	// other approach directions after failures (blocked tiles next to the target)
	double fallback = 0.785398 * a->chase_failures;
	Point dest = approach_point(ground(apos), a->stats->radius, footprint(*t->stats, tpos), gap, fallback);
	if (a->chase_failures > 0) {
		// rotate around the target center
		Point c = ground(tpos);
		double ang = std::atan2(dest.y - c.y, dest.x - c.x) + fallback;
		double r = distance(dest, c);
		dest = {c.x + std::cos(ang) * r, c.y + std::sin(ang) * r};
	}
	bool reorder = not a->chase_destination or not moving
	               or distance(dest, ground(*a->chase_destination)) > 0.75;
	if (reorder) {
		auto map = state->get_map();
		coord::phys3 target_pos{coord::phys_t{dest.x}, coord::phys_t{dest.y}, coord::phys_t{0.0}};
		target_pos = map->on_terrain(target_pos);
		auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
			a->entity->get_component(component::component_t::COMMANDQUEUE));
		queue->add_command(time, std::make_shared<component::command::MoveCommand>(target_pos));
		a->chase_destination = target_pos;
		a->chase_ordered = time;
	}
	this->schedule_tick(state, *a, time + seconds(CHASE_PERIOD));
}

void CombatState::hit(const std::shared_ptr<GameState> &state,
                      Combatant &attacker,
                      Combatant &target,
                      const time::time_t &time) {
	auto live = std::dynamic_pointer_cast<component::Live>(
		target.entity->get_component(component::component_t::LIVE));
	auto current = live->get_attribute(time, target.stats->health_attribute);
	if (not current) {
		return;
	}
	auto damage = compute_damage(attacker.stats->attack, target.stats->armor);
	auto health = std::max<int64_t>(0, *current - damage);
	live->set_attribute(time, target.stats->health_attribute, health);
	this->set_health_snapshot(target.entity->get_id(), health, target.stats->max_health, health > 0);

	auto attacker_owner = this->owner_of(attacker.entity, time);
	auto target_owner = this->owner_of(target.entity, time);
	if (this->allow_hit_log(time) or not attacker.hit_logged) {
		attacker.hit_logged = true;
		log::log(INFO << "Combat: " << describe(attacker.entity, *attacker.stats, attacker_owner)
		              << " hits " << describe(target.entity, *target.stats, target_owner)
		              << " for " << damage << ", health " << health << "/" << target.stats->max_health
		              << " (t=" << time.to_double() << ")");
	}

	if (health <= 0) {
		this->die(state, target, time, describe(attacker.entity, *attacker.stats, attacker_owner));
		return;
	}

	// fight back (military units without a target)
	if (not target.target and target.stats->can_attack and not target.stats->villager
	    and (target.stats->movable or attacker.stats->ranged or target.stats->ranged)) {
		this->engage(state, target, attacker.entity->get_id(), false, time);
	}
}

void CombatState::die(const std::shared_ptr<GameState> &state,
                      Combatant &victim,
                      const time::time_t &time,
                      const std::string &cause) {
	if (victim.dead) {
		return;
	}
	victim.dead = true;
	victim.target.reset();
	if (victim.tick) {
		victim.tick->cancel(time);
		victim.tick = nullptr;
	}

	// stop moving and acting
	auto pos = std::dynamic_pointer_cast<component::Position>(
		victim.entity->get_component(component::component_t::POSITION));
	pos->set_position(time, pos->get_positions().get(time));
	auto activity = std::dynamic_pointer_cast<component::Activity>(
		victim.entity->get_component(component::component_t::ACTIVITY));
	activity->cancel_events(time);

	if (not victim.stats->death_animation.empty()) {
		victim.entity->render_update(time, victim.stats->death_animation);
	}
	auto owner = this->owner_of(victim.entity, time);
	log::log(INFO << "Combat: " << describe(victim.entity, *victim.stats, owner) << " died"
	              << (cause.empty() ? std::string{} : " (killed by " + cause + ")")
	              << " at t=" << time.to_double());
	this->set_health_snapshot(victim.entity->get_id(), 0, victim.stats->max_health, false);

	openage::event::EventHandler::param_map::map_t params{{"entity", victim.entity->get_id()}};
	this->loop->create_event("game.combat_remove",
	                         std::static_pointer_cast<CombatState>(state->get_combat()),
	                         state,
	                         time + seconds(std::max(0.05, victim.stats->death_time)),
	                         params);
	this->update_match(time);
}

void CombatState::kill(const std::shared_ptr<GameState> &state,
                       entity_id_t id,
                       const time::time_t &time) {
	auto c = this->find(id);
	if (not c or c->dead) {
		return;
	}
	auto live = std::dynamic_pointer_cast<component::Live>(
		c->entity->get_component(component::component_t::LIVE));
	live->set_attribute(time, c->stats->health_attribute, 0);
	this->die(state, *c, time, "");
}

void CombatState::remove(const std::shared_ptr<GameState> &state, entity_id_t id, const time::time_t &time) {
	auto c = this->find(id);
	if (not c) {
		return;
	}
	auto entity = c->entity;
	if (c->stats->building) {
		// free the footprint for pathfinding (whole tiles under the square)
		auto pos = this->position_of(entity, time);
		double r = c->stats->radius;
		std::vector<coord::tile> tiles;
		auto ne0 = static_cast<coord::tile_t>(std::floor(pos.ne.to_double() - r + 0.01));
		auto ne1 = static_cast<coord::tile_t>(std::ceil(pos.ne.to_double() + r - 0.01));
		auto se0 = static_cast<coord::tile_t>(std::floor(pos.se.to_double() - r + 0.01));
		auto se1 = static_cast<coord::tile_t>(std::ceil(pos.se.to_double() + r - 0.01));
		for (auto se = se0; se < se1; ++se) {
			for (auto ne = ne0; ne < ne1; ++ne) {
				tiles.push_back(coord::tile{ne, se});
			}
		}
		state->get_map()->unblock_tiles(tiles, time);
	}
	entity->remove_render_entity();
	state->remove_game_entity(id);
	this->combatants.erase(id);
	{
		std::lock_guard<std::mutex> lock{this->shared_mutex};
		this->removed.insert(id);
		this->health_snapshot.erase(id);
	}
	log::log(DBG << "Combat: entity " << id << " removed at t=" << time.to_double());
}

void CombatState::scan(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	this->update_match(time);

	std::vector<entity_id_t> ids;
	ids.reserve(this->combatants.size());
	for (const auto &[id, c] : this->combatants) {
		if (not c.dead and c.stats->can_attack and not c.target and not c.stats->villager) {
			ids.push_back(id);
		}
	}
	std::sort(ids.begin(), ids.end());
	for (auto id : ids) {
		auto c = this->find(id);
		if (not c or c->dead or c->target) {
			continue;
		}
		auto owner = this->owner_of(c->entity, time);
		if (this->neutral.contains(owner)) {
			continue;
		}
		double range = c->stats->max_range;
		if (c->stats->movable) {
			if (this->is_moving(c->entity, time)) {
				continue;
			}
			range = std::max(c->stats->line_of_sight, c->stats->max_range);
		}
		auto enemy = this->nearest_enemy(*c, range, time);
		if (enemy) {
			auto e = this->find(*enemy);
			if (this->allow_hit_log(time)) {
				log::log(INFO << "Combat: auto attack " << describe(c->entity, *c->stats, owner)
				              << " -> " << describe(e->entity, *e->stats, this->owner_of(e->entity, time)));
			}
			this->engage(state, *c, *enemy, false, time);
		}
	}

	this->loop->create_event("game.combat_scan",
	                         std::static_pointer_cast<CombatState>(state->get_combat()),
	                         state,
	                         time + seconds(SCAN_PERIOD),
	                         openage::event::EventHandler::param_map::map_t{});
}

void CombatState::update_match(const time::time_t &time) {
	std::map<player_id_t, size_t> alive;
	for (auto player : this->participants) {
		alive[player] = 0;
	}
	for (const auto &[id, c] : this->combatants) {
		if (c.dead or not c.stats->counts_for_victory()) {
			continue;
		}
		auto owner = this->owner_of(c.entity, time);
		if (this->neutral.contains(owner)) {
			continue;
		}
		alive[owner] += 1;
		this->participants.insert(owner);
	}
	auto result = evaluate_match(alive);

	std::vector<player_id_t> newly_defeated;
	MatchStatus status_copy;
	std::vector<std::function<void(const MatchStatus &)>> callbacks;
	bool decided_now = false;
	{
		std::lock_guard<std::mutex> lock{this->shared_mutex};
		for (auto player : result.defeated) {
			const auto &old = this->match_status.result.defeated;
			if (std::find(old.begin(), old.end(), player) == old.end()) {
				newly_defeated.push_back(player);
			}
		}
		this->match_status.alive = alive;
		this->match_status.result = result;
		if (result.over and not this->match_over) {
			this->match_over = true;
			this->match_status.decided_at = time;
			status_copy = this->match_status;
			callbacks = this->match_callbacks;
			decided_now = true;
		}
	}
	for (auto player : newly_defeated) {
		log::log(INFO << "Combat: player " << player << " has no units or buildings left (defeat)"
		              << " at t=" << time.to_double());
	}
	if (decided_now) {
		if (result.winner) {
			log::log(INFO << "Combat: match over, player " << *result.winner << " wins (victory) at t="
			              << time.to_double());
		}
		else {
			log::log(INFO << "Combat: match over, draw at t=" << time.to_double());
		}
		for (const auto &callback : callbacks) {
			callback(status_copy);
		}
	}
}

// ---------------------------------------------------------------- queries

bool CombatState::is_dead(entity_id_t id) const {
	auto c = this->find(id);
	return c and c->dead;
}

std::shared_ptr<const CombatStats> CombatState::get_stats(entity_id_t id) const {
	auto c = this->find(id);
	return c ? c->stats : nullptr;
}

std::optional<entity_id_t> CombatState::get_target(entity_id_t id) const {
	auto c = this->find(id);
	return c ? c->target : std::nullopt;
}

bool CombatState::was_removed(entity_id_t id) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	return this->removed.contains(id);
}

std::vector<HealthInfo> CombatState::get_health(const std::vector<entity_id_t> &ids) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	std::vector<HealthInfo> result;
	result.reserve(ids.size());
	for (auto id : ids) {
		auto it = this->health_snapshot.find(id);
		if (it == this->health_snapshot.end()) {
			result.push_back({id, 0, 0, false});
		}
		else {
			result.push_back(it->second);
		}
	}
	return result;
}

MatchStatus CombatState::get_match_status() const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	return this->match_status;
}

match_state_t CombatState::get_match_state(player_id_t player) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	return this->match_status.result.state_for(player);
}

void CombatState::on_match_over(std::function<void(const MatchStatus &)> callback) {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	this->match_callbacks.push_back(std::move(callback));
}

} // namespace openage::gamestate::combat
