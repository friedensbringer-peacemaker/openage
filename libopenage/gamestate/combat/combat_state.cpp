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
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/constructable.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/api/move.h"
#include "gamestate/component/internal/activity.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/build.h"
#include "gamestate/component/internal/commands/gather.h"
#include "gamestate/component/internal/commands/move.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/manager.h"
#include "gamestate/map.h"
#include "gamestate/player.h"
#include "gamestate/system/idle.h"
#include "gamestate/tech.h"
#include "gamestate/vision/vision_state.h"


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
/// garrison: a unit enters when its edge is this close to the building (tiles)
constexpr double ENTER_DISTANCE = 0.6;
/// buildings below this share of their health throw the garrison out (06 §6.5)
constexpr double EJECT_HEALTH = 0.2;
/// order logic: move commands at most this often (s)
constexpr double ORDER_MOVE_PERIOD = 2.0;
/// unit class of the towers (05 §5.4, 06 §6.4)
constexpr int CLASS_TOWER = 52;
/// unit class of the scout cavalry (06 §6.2: no automatic attacks on villagers)
constexpr int CLASS_SCOUT = 47;
/// herbal medicine: garrisoned units heal this much faster (public knowledge)
constexpr double HERBAL_MEDICINE = 4.0;

time::time_t seconds(double s) {
	return time::time_t::from_double(s);
}

Point ground(const coord::phys3 &pos) {
	return {pos.ne.to_double(), pos.se.to_double()};
}

coord::phys3 phys(const Point &p) {
	return coord::phys3{coord::phys_t{p.x}, coord::phys_t{p.y}, coord::phys_t{0.0}};
}

template <typename T>
std::shared_ptr<T> component_of(const std::shared_ptr<GameEntity> &entity, component::component_t type) {
	if (not entity->has_component(type)) {
		return nullptr;
	}
	return std::dynamic_pointer_cast<T>(entity->get_component(type));
}

} // namespace


CombatState::CombatState(const std::shared_ptr<openage::event::EventLoop> &loop) :
	openage::event::EventEntity{loop},
	loop{loop},
	hidden_snapshot{std::make_shared<const std::unordered_set<entity_id_t>>()} {
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
	if (auto dot = nyan_entity.find('.'); dot != std::string::npos and nyan_entity.compare(0, dot, "engine") != 0) {
		this->modpack = nyan_entity.substr(0, dot);
	}
	auto cached = this->stats_cache.find(key.str());
	std::shared_ptr<const CombatStats> stats;
	if (cached == this->stats_cache.end()) {
		stats = read_combat_stats(db_view, nyan_entity);
		this->stats_cache.emplace(key.str(), stats);
		log::log(DBG << "Combat stats " << stats->name << ": health " << stats->max_health
		             << ", attack " << attack_sum(stats->attack) << (stats->ranged ? " ranged" : "")
		             << ", reload " << stats->reload_time << " s, range " << stats->max_range
		             << ", sight " << stats->line_of_sight << ", radius " << stats->radius
		             << ", class " << stats->unit_class << ", garrison " << stats->garrison_capacity
		             << "/" << stats->garrison_mask << ", arrows " << stats->min_projectiles << "-"
		             << stats->max_projectiles);
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
	{
		std::lock_guard<std::mutex> lock{this->shared_mutex};
		this->stats_snapshot[entity->get_id()] = stats;
	}
	this->publish_info(this->combatants[entity->get_id()]);
}

void CombatState::start(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	if (this->started) {
		return;
	}
	this->started = true;
	this->last_scan = time.to_double();
	this->loop->create_event("game.combat_scan",
	                         std::static_pointer_cast<CombatState>(state->get_combat()),
	                         state,
	                         time + seconds(SCAN_PERIOD),
	                         openage::event::EventHandler::param_map::map_t{});
}

void CombatState::set_neutral_players(const std::unordered_set<player_id_t> &players) {
	this->neutral = players;
}

void CombatState::set_vision(const std::shared_ptr<vision::VisionState> &vision) {
	this->vision = vision;
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
	if (a.entity == b.entity or b.dead or not b.stats->attackable() or b.inside) {
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
	// XR fork (vision): only enemies the owner sees
	const auto self_owner = this->vision ? this->owner_of(self.entity, time) : player_id_t{0};
	for (const auto &[id, other] : this->combatants) {
		if (not this->is_enemy(self, other, time)) {
			continue;
		}
		if (this->vision) {
			auto pos = this->position_of(other.entity, time);
			if (not this->vision->sees(self_owner, pos.ne.to_double(), pos.se.to_double(),
			                           other.stats->building ? other.stats->radius : 0.0)) {
				continue;
			}
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

double CombatState::from_post(const Combatant &self, const Point &p, const time::time_t &time) const {
	if (self.order == order_t::PATROL and self.route.size() >= 2) {
		return route_distance(p, self.route[0], self.route[1]);
	}
	if (self.order == order_t::GUARD and self.order_entity) {
		if (auto *g = this->find(*self.order_entity); g and not g->dead) {
			return distance(p, ground(this->position_of(g->entity, time)));
		}
	}
	const Point post = self.post ? *self.post : ground(this->position_of(self.entity, time));
	return distance(p, post);
}

std::optional<entity_id_t> CombatState::choose_target(const Combatant &self, const time::time_t &time) const {
	const auto stance = self.stats->building ? stance_t::AGGRESSIVE : self.stance;
	if (stance == stance_t::NO_ATTACK or self.order == order_t::FOLLOW or self.inside) {
		return std::nullopt;
	}
	const double range = self.stats->max_range > 0.0 ? self.stats->max_range : MELEE_RANGE;
	const double radius = self.stats->movable ? scan_radius(self.stats->line_of_sight, self.stats->max_range)
	                                          : self.stats->max_range;
	const double now = time.to_double();
	const auto self_owner = this->owner_of(self.entity, time);
	const auto self_pos = this->position_of(self.entity, time);
	std::vector<Candidate> cands;
	for (const auto &[id, other] : this->combatants) {
		if (not this->is_enemy(self, other, time)) {
			continue;
		}
		// buildings (and walls, gates, towers) are no automatic targets (06 §6.2)
		if (other.stats->building) {
			continue;
		}
		if (self.stats->unit_class == CLASS_SCOUT and other.stats->villager) {
			continue;
		}
		if (auto lock = self.locks.find(id); lock != self.locks.end() and lock->second > now) {
			continue;
		}
		auto pos = this->position_of(other.entity, time);
		if (this->vision and not this->vision->sees(self_owner, pos.ne.to_double(), pos.se.to_double(), 0.0)) {
			continue;
		}
		const double edge = this->edge_between(self, other, time);
		if (edge > radius) {
			continue;
		}
		if (not self.stats->movable or not stance_moves(stance)) {
			// fixed attackers (buildings, stand ground) only what is in range now
			if (not in_attack_range(edge, self.stats->min_range, self.stats->max_range)) {
				continue;
			}
		}
		if (self.stats->movable) {
			// the stance limits the distance from the post (route, guarded object)
			const auto target_point = nearest_point(footprint(*other.stats, pos), ground(self_pos));
			if (not stance_allows(stance, this->from_post(self, target_point, time), range)) {
				continue;
			}
		}
		Candidate c;
		c.id = id;
		c.distance = edge;
		const auto damage = compute_damage(self.stats->attack, other.stats->armor);
		auto health = other.stats->max_health;
		if (auto live = component_of<component::Live>(other.entity, component::component_t::LIVE)) {
			health = live->get_attribute(time, other.stats->health_attribute).value_or(health);
		}
		c.kill_time = kill_time(static_cast<double>(health), static_cast<double>(damage), self.stats->reload_time);
		c.category = category_of(other.stats->unit_class, other.stats->building, other.stats->villager);
		c.current = self.target and *self.target == id;
		c.movable = other.stats->movable;
		cands.push_back(c);
	}
	if (cands.empty()) {
		return std::nullopt;
	}
	std::array<double, CATEGORY_COUNT> threat{};
	if (auto it = this->threats.find(self_owner); it != this->threats.end()) {
		threat = it->second.normalized(now);
	}
	auto best = best_targets(cands, threat, TARGET_TRIES);
	return best.empty() ? std::nullopt : std::optional<entity_id_t>{cands[best.front()].id};
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

std::string describe(entity_id_t id, const CombatStats &stats, player_id_t owner) {
	std::ostringstream out;
	out << stats.name << " " << id << " (P" << owner << ")";
	return out.str();
}

} // namespace

void CombatState::set_health_snapshot(entity_id_t id, int64_t health, int64_t max_health, bool alive) {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	this->health_snapshot[id] = {id, health, max_health, alive};
}

void CombatState::health_changed(entity_id_t id, int64_t health) {
	auto *combatant = this->find(id);
	if (combatant == nullptr or combatant->dead) {
		return;
	}
	this->set_health_snapshot(id, health, combatant->stats->max_health, health > 0);
}

// XR fork W1: technologies patch the view of a player
void CombatState::invalidate_stats(const std::shared_ptr<nyan::View> &db_view) {
	std::ostringstream suffix;
	suffix << "@" << db_view.get();
	const auto tail = suffix.str();
	for (auto it = this->stats_cache.begin(); it != this->stats_cache.end();) {
		const auto &key = it->first;
		if (key.size() >= tail.size() and key.compare(key.size() - tail.size(), tail.size(), tail) == 0) {
			it = this->stats_cache.erase(it);
		}
		else {
			++it;
		}
	}
}

bool CombatState::refresh_entity(const std::shared_ptr<GameEntity> &entity,
                                 const std::shared_ptr<nyan::View> &db_view,
                                 const nyan::fqon_t &nyan_entity,
                                 bool under_construction,
                                 bool proportional,
                                 const time::time_t &time) {
	auto *combatant = this->find(entity->get_id());
	if (combatant == nullptr or combatant->dead) {
		return false;
	}
	std::ostringstream key;
	key << nyan_entity << "@" << db_view.get();
	std::shared_ptr<const CombatStats> stats;
	auto cached = this->stats_cache.find(key.str());
	if (cached == this->stats_cache.end()) {
		stats = read_combat_stats(db_view, nyan_entity);
		this->stats_cache.emplace(key.str(), stats);
	}
	else {
		stats = cached->second;
	}
	const auto old = combatant->stats;
	combatant->stats = stats;
	{
		std::lock_guard<std::mutex> lock{this->shared_mutex};
		this->stats_snapshot[entity->get_id()] = stats;
	}
	this->publish_info(*combatant);
	auto live = std::dynamic_pointer_cast<component::Live>(entity->get_component(component::component_t::LIVE));
	if (live == nullptr or stats->health_attribute.empty()) {
		return false;
	}
	auto current = live->get_attribute(time, stats->health_attribute);
	int64_t health = current ? *current : stats->max_health;
	const bool changed = old == nullptr or old->max_health != stats->max_health;
	if (changed and not under_construction) {
		const int64_t old_max = old ? old->max_health : stats->max_health;
		if (proportional and old_max > 0) {
			// replacement of the type (AoE II): health scaled with the maximum
			health = static_cast<int64_t>(std::llround(static_cast<double>(health) * static_cast<double>(stats->max_health)
			                                           / static_cast<double>(old_max)));
		}
		else {
			// attribute change (AoE II, e.g. Loom): the current health rises with the maximum
			health += std::max<int64_t>(0, stats->max_health - old_max);
		}
		health = std::clamp<int64_t>(health, 1, std::max<int64_t>(1, stats->max_health));
		live->set_attribute(time, stats->health_attribute, health);
	}
	this->set_health_snapshot(entity->get_id(), health, stats->max_health, health > 0);
	return changed;
}

// ---------------------------------------------------------------- orders

bool CombatState::order_attack(const std::shared_ptr<GameState> &state,
                               entity_id_t attacker,
                               entity_id_t target,
                               const time::time_t &time) {
	auto a = this->find(attacker);
	auto t = this->find(target);
	if (not a or not t or a->dead or a->inside or not a->stats->can_attack or not this->is_enemy(*a, *t, time)) {
		return false;
	}
	if (a->stats->movable) {
		// stop what the unit does (walking, gathering): the attack ticks take over
		this->halt(a->entity, time);
	}
	this->clear_order(*a);
	a->entering.reset();
	a->locks.erase(target);
	this->engage(state, *a, target, true, time);
	this->publish_info(*a);
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

void CombatState::clear_order(Combatant &c) {
	c.order = order_t::NONE;
	c.route.clear();
	c.leg = 0;
	c.order_entity.reset();
	c.order_dest.reset();
	c.order_moved = time::TIME_MIN;
}

void CombatState::player_moved(entity_id_t id, const std::optional<coord::phys3> &destination, const time::time_t &time) {
	auto *c = this->find(id);
	if (c == nullptr or c->dead) {
		return;
	}
	this->cancel_attack(id, time);
	const bool had_order = c->order != order_t::NONE or c->entering.has_value();
	this->clear_order(*c);
	c->entering.reset();
	c->fleeing = false;
	c->return_to_post = false;
	c->resume_gather.reset();
	c->resume_build.reset();
	if (destination) {
		c->post = ground(*destination);
	}
	else if (c->entity->has_component(component::component_t::POSITION)) {
		c->post = ground(this->position_of(c->entity, time));
	}
	if (had_order) {
		this->publish_info(*c);
	}
}

void CombatState::stop(const std::shared_ptr<GameState> & /* state */, entity_id_t id, const time::time_t &time) {
	auto *c = this->find(id);
	if (c == nullptr or c->dead or c->inside or not c->stats->movable) {
		return;
	}
	this->cancel_attack(id, time);
	this->clear_order(*c);
	c->entering.reset();
	c->fleeing = false;
	this->halt(c->entity, time);
	c->post = ground(this->position_of(c->entity, time));
	this->publish_info(*c);
}

bool CombatState::set_stance(entity_id_t id, stance_t stance) {
	auto *c = this->find(id);
	if (c == nullptr or c->dead or not c->stats->stances) {
		return false;
	}
	c->stance = stance;
	this->publish_info(*c);
	return true;
}

bool CombatState::set_formation(entity_id_t id, formation_t formation) {
	auto *c = this->find(id);
	if (c == nullptr or c->dead or not c->stats->formations or not c->stats->military()) {
		return false;
	}
	c->formation = formation;
	this->publish_info(*c);
	return true;
}

std::vector<entity_id_t> CombatState::formation_move(const std::shared_ptr<GameState> &state,
                                                     const std::vector<entity_id_t> &ids,
                                                     const coord::phys3 &destination,
                                                     const time::time_t &time) {
	std::vector<FormationMember> members;
	std::vector<double> speeds;
	formation_t formation = formation_t::NONE;
	Point centroid{0.0, 0.0};
	for (auto id : ids) {
		auto *c = this->find(id);
		if (c == nullptr or c->dead or c->inside or not c->stats->military() or c->formation == formation_t::NONE) {
			continue;
		}
		if (formation == formation_t::NONE) {
			formation = c->formation;
		}
		const auto pos = ground(this->position_of(c->entity, time));
		members.push_back({id, c->stats->formation_priority, pos});
		speeds.push_back(c->stats->speed);
		centroid.x += pos.x;
		centroid.y += pos.y;
	}
	if (members.size() < 2) {
		return {};
	}
	centroid.x /= static_cast<double>(members.size());
	centroid.y /= static_cast<double>(members.size());
	const Point dest = ground(destination);
	Point dir{dest.x - centroid.x, dest.y - centroid.y};
	auto slots = formation_slots(formation, members, dest, dir);
	const double speed = group_speed(speeds);
	auto map = state->get_map();
	// slots on blocked tiles (trees, buildings) move to the nearest free tile no other slot uses
	std::optional<path::grid_id_t> grid;
	if (auto move = component_of<component::Move>(this->find(members.front().id)->entity, component::component_t::MOVE)) {
		try {
			grid = map->get_grid_id(move->get_ability().get<nyan::ObjectValue>("Move.path_type")->get_name());
		}
		catch (std::exception &) {
		}
	}
	if (grid) {
		std::vector<Point> taken;
		for (auto &[id, slot] : slots) {
			auto free = [&](const Point &p) {
				if (not map->is_passable(*grid, phys(p).to_tile())) {
					return false;
				}
				for (const auto &t : taken) {
					if (distance(t, p) < 0.7) {
						return false;
					}
				}
				return true;
			};
			if (not free(slot)) {
				bool found = false;
				for (int r = 1; r <= 6 and not found; ++r) {
					for (int i = 0; i < 8 * r and not found; ++i) {
						const double a = 6.283185307 * static_cast<double>(i) / static_cast<double>(8 * r);
						Point p{slot.x + std::cos(a) * r * 0.75, slot.y + std::sin(a) * r * 0.75};
						if (free(p)) {
							slot = p;
							found = true;
						}
					}
				}
			}
			taken.push_back(slot);
		}
	}
	std::vector<entity_id_t> moved;
	for (const auto &[id, slot] : slots) {
		auto *c = this->find(id);
		this->player_moved(id, phys(slot), time);
		auto queue = component_of<component::CommandQueue>(c->entity, component::component_t::COMMANDQUEUE);
		if (queue) {
			queue->add_command(time, std::make_shared<component::command::MoveCommand>(map->on_terrain(phys(slot)), speed));
		}
		moved.push_back(id);
	}
	log::log(INFO << "Combat: formation " << to_string(formation) << " of " << moved.size()
	              << " units to tile (" << dest.x << ", " << dest.y << "), speed " << speed);
	return moved;
}

bool CombatState::order_patrol(const std::shared_ptr<GameState> &state, entity_id_t id, const coord::phys3 &point,
                               const time::time_t &time) {
	auto *c = this->find(id);
	if (c == nullptr or c->dead or c->inside or not c->stats->military()) {
		return false;
	}
	this->player_moved(id, std::nullopt, time);
	c->order = order_t::PATROL;
	c->route = {ground(this->position_of(c->entity, time)), this->passable_near(state, *c, ground(point))};
	c->leg = 1;
	this->order_move(state, *c, c->route[1], time, true);
	this->publish_info(*c);
	return true;
}

bool CombatState::order_guard(const std::shared_ptr<GameState> &state, entity_id_t id, entity_id_t guarded,
                              const time::time_t &time) {
	auto *c = this->find(id);
	auto *g = this->find(guarded);
	if (c == nullptr or g == nullptr or c->dead or g->dead or c->inside or id == guarded
	    or not c->stats->military() or this->owner_of(c->entity, time) != this->owner_of(g->entity, time)) {
		return false;
	}
	this->player_moved(id, std::nullopt, time);
	c->order = order_t::GUARD;
	c->order_entity = guarded;
	this->update_order(state, *c, time);
	this->publish_info(*c);
	return true;
}

bool CombatState::order_follow(const std::shared_ptr<GameState> &state, entity_id_t id, entity_id_t followed,
                               const time::time_t &time) {
	auto *c = this->find(id);
	auto *f = this->find(followed);
	if (c == nullptr or f == nullptr or c->dead or f->dead or c->inside or id == followed or not c->stats->movable
	    or f->stats->building) {
		return false;
	}
	this->player_moved(id, std::nullopt, time);
	c->order = order_t::FOLLOW;
	c->order_entity = followed;
	this->update_order(state, *c, time);
	this->publish_info(*c);
	return true;
}

bool CombatState::order_attack_move(const std::shared_ptr<GameState> &state, entity_id_t id, const coord::phys3 &point,
                                    const time::time_t &time) {
	auto *c = this->find(id);
	if (c == nullptr or c->dead or c->inside or not c->stats->military()) {
		return false;
	}
	const Point dest = this->passable_near(state, *c, ground(point));
	this->player_moved(id, phys(dest), time);
	c->order = order_t::ATTACK_MOVE;
	c->route = {dest};
	this->order_move(state, *c, c->route[0], time, true);
	this->publish_info(*c);
	return true;
}

Point CombatState::passable_near(const std::shared_ptr<GameState> &state, const Combatant &c, const Point &p) const {
	auto map = state->get_map();
	std::optional<path::grid_id_t> grid;
	if (auto move = component_of<component::Move>(c.entity, component::component_t::MOVE)) {
		try {
			grid = map->get_grid_id(move->get_ability().get<nyan::ObjectValue>("Move.path_type")->get_name());
		}
		catch (std::exception &) {
		}
	}
	if (not grid or map->is_passable(*grid, phys(p).to_tile())) {
		return p;
	}
	// nearest free tile around the point (buildings, trees), on each ring the one nearest to the unit
	const auto from = ground(this->position_of(c.entity, this->clock));
	const auto size = map->get_size();
	for (int r = 1; r <= 10; ++r) {
		std::optional<Point> best;
		double best_d = 1e30;
		for (int dn = -r; dn <= r; ++dn) {
			for (int ds = -r; ds <= r; ++ds) {
				if (std::max(std::abs(dn), std::abs(ds)) != r) {
					continue;
				}
				const Point q{std::floor(p.x) + dn + 0.5, std::floor(p.y) + ds + 0.5};
				if (q.x < 0.0 or q.y < 0.0 or q.x >= static_cast<double>(size[0]) or q.y >= static_cast<double>(size[1])) {
					continue;
				}
				if (not map->is_passable(*grid, phys(q).to_tile())) {
					continue;
				}
				const double d = distance(q, from);
				if (d < best_d) {
					best_d = d;
					best = q;
				}
			}
		}
		if (best) {
			return *best;
		}
	}
	return p;
}

void CombatState::order_move(const std::shared_ptr<GameState> &state, Combatant &c, const Point &dest,
                             const time::time_t &time, bool force) {
	if (not force and c.order_dest and distance(*c.order_dest, dest) < 0.5
	    and time - c.order_moved < seconds(ORDER_MOVE_PERIOD)) {
		return;
	}
	auto queue = component_of<component::CommandQueue>(c.entity, component::component_t::COMMANDQUEUE);
	if (not queue) {
		return;
	}
	auto target = state->get_map()->on_terrain(phys(dest));
	queue->add_command(time, std::make_shared<component::command::MoveCommand>(target));
	c.order_dest = dest;
	c.order_moved = time;
}

void CombatState::update_order(const std::shared_ptr<GameState> &state, Combatant &c, const time::time_t &time) {
	if (c.dead or c.inside or c.entering or not c.stats->movable) {
		return;
	}
	const auto pos = ground(this->position_of(c.entity, time));
	const bool moving = this->is_moving(c.entity, time);
	switch (c.order) {
	case order_t::PATROL: {
		if (c.target or c.route.size() < 2) {
			return;
		}
		// arrived (or standing close to it: a crowd of units cannot all reach one point)
		const double dleg = distance(pos, c.route[c.leg]);
		if (dleg <= PATROL_ARRIVE or (not moving and dleg <= 2.0)) {
			c.leg = 1 - c.leg;
			this->order_move(state, c, c.route[c.leg], time, true);
		}
		else if (not moving) {
			this->order_move(state, c, c.route[c.leg], time, true);
		}
		return;
	}
	case order_t::ATTACK_MOVE: {
		if (c.target or c.route.empty()) {
			return;
		}
		const double dend = distance(pos, c.route[0]);
		if (dend <= PATROL_ARRIVE or (not moving and dend <= 3.0 and time - c.order_moved > seconds(1.0))) {
			c.post = pos;
			this->clear_order(c);
			this->publish_info(c);
			return;
		}
		if (not moving) {
			this->order_move(state, c, c.route[0], time, true);
		}
		return;
	}
	case order_t::GUARD:
	case order_t::FOLLOW: {
		auto *o = c.order_entity ? this->find(*c.order_entity) : nullptr;
		if (o == nullptr or o->dead or o->inside) {
			// the guarded/followed entity is gone: stay where it is
			c.post = pos;
			this->clear_order(c);
			this->publish_info(c);
			return;
		}
		if (c.target) {
			return;
		}
		const auto opos = ground(this->position_of(o->entity, time));
		const double keep = c.order == order_t::GUARD ? GUARD_DISTANCE + o->stats->radius : FOLLOW_DISTANCE;
		const double d = distance(pos, opos);
		if (d > keep + 0.75) {
			// to a point at the distance on the line from the object to the unit
			const double f = keep / std::max(d, 1e-6);
			Point dest{opos.x + (pos.x - opos.x) * f, opos.y + (pos.y - opos.y) * f};
			this->order_move(state, c, dest, time, not moving);
		}
		c.post = opos;
		return;
	}
	case order_t::ATTACK:
	case order_t::NONE:
	default:
		break;
	}
	// stance leash: back to the post (06 §6.2); after an automatic fight a defensive unit
	// walks back as well
	if (not c.target and c.post and not moving and c.stats->military()) {
		const double d = distance(pos, *c.post);
		if (stance_returns(c.stance, d) or (c.return_to_post and d > 1.0)) {
			this->order_move(state, c, *c.post, time);
		}
		if (d <= 1.0) {
			c.return_to_post = false;
		}
	}
}

// ---------------------------------------------------------------- garrison (05 §5.4, 06 §6.5)

bool CombatState::finished(const Combatant &c) const {
	auto constructable = component_of<component::Constructable>(c.entity, component::component_t::CONSTRUCTABLE);
	return constructable == nullptr or constructable->is_complete();
}

bool CombatState::accepts(const Combatant &building, const Combatant &unit, const time::time_t &time) const {
	if (building.dead or unit.dead or not building.stats->building or building.stats->garrison_capacity <= 0
	    or not this->finished(building)) {
		return false;
	}
	if (this->owner_of(building.entity, time) != this->owner_of(unit.entity, time)) {
		return false;
	}
	if (static_cast<int>(building.occupants.size()) >= building.stats->garrison_capacity) {
		return false;
	}
	return garrison_accepts(building.stats->garrison_mask, garrison_kind(unit.stats->unit_class, unit.stats->villager));
}

bool CombatState::order_garrison(const std::shared_ptr<GameState> &state, entity_id_t id, entity_id_t building,
                                 const time::time_t &time, bool by_bell) {
	auto *c = this->find(id);
	auto *b = this->find(building);
	if (c == nullptr or b == nullptr or c->inside or not c->stats->movable or not this->accepts(*b, *c, time)) {
		return false;
	}
	this->player_moved(id, std::nullopt, time);
	c->by_bell = by_bell;
	if (by_bell) {
		// the bell keeps the job and the place (back to work afterwards)
		this->remember_job(*c);
		c->bell_from = ground(this->position_of(c->entity, time));
	}
	c->entering = building;
	this->halt(c->entity, time);
	this->schedule_tick(state, *c, time);
	this->publish_info(*c);
	return true;
}

void CombatState::enter(const std::shared_ptr<GameState> &state, Combatant &unit, Combatant &building,
                        const time::time_t &time) {
	unit.entering.reset();
	unit.inside = building.entity->get_id();
	unit.target.reset();
	unit.fleeing = false;
	if (unit.tick) {
		unit.tick->cancel(time);
		unit.tick = nullptr;
	}
	this->clear_order(unit);
	// stand in the building (hidden, no sight of its own, see vision_state.cpp)
	auto pos = component_of<component::Position>(unit.entity, component::component_t::POSITION);
	auto activity = component_of<component::Activity>(unit.entity, component::component_t::ACTIVITY);
	if (activity) {
		activity->cancel_events(time);
	}
	if (pos) {
		pos->set_position(time, state->get_map()->on_terrain(this->position_of(building.entity, time)));
	}
	building.occupants.push_back(unit.entity->get_id());
	log::log(INFO << "Combat: " << describe(unit.entity, *unit.stats, this->owner_of(unit.entity, time))
	              << " garrisoned in " << describe(building.entity, *building.stats, this->owner_of(building.entity, time))
	              << (unit.by_bell ? " (town bell)" : "") << ", " << building.occupants.size() << "/"
	              << building.stats->garrison_capacity << ", arrows "
	              << arrow_count(this->running_arrows(building, time), building.stats->max_projectiles));
	this->publish_info(unit);
	this->publish_info(building);
	this->publish_hidden();
	// a building with a new garrison shoots at enemies at once
	if (building.stats->can_attack and not building.target) {
		if (auto enemy = this->choose_target(building, time)) {
			this->engage(state, building, *enemy, false, time);
		}
	}
}

void CombatState::leave(const std::shared_ptr<GameState> &state, Combatant &unit, Combatant &building, size_t slot,
                        const time::time_t &time) {
	unit.inside.reset();
	std::erase(building.occupants, unit.entity->get_id());
	auto map = state->get_map();
	const auto bpos = ground(this->position_of(building.entity, time));
	// a free spot around the building (golden angle steps, then wider rings)
	std::optional<path::grid_id_t> grid;
	if (auto move = component_of<component::Move>(unit.entity, component::component_t::MOVE)) {
		try {
			auto path_type = move->get_ability().get<nyan::ObjectValue>("Move.path_type");
			grid = map->get_grid_id(path_type->get_name());
		}
		catch (std::exception &) {
		}
	}
	Point spot = bpos;
	const double base = building.stats->radius + unit.stats->radius + 0.45;
	bool found = false;
	for (int ring = 0; ring < 4 and not found; ++ring) {
		for (int i = 0; i < 24 and not found; ++i) {
			const double angle = 2.39996323 * static_cast<double>(slot + static_cast<size_t>(i) + static_cast<size_t>(ring) * 7);
			const double r = base + building.stats->radius * (std::sqrt(2.0) - 1.0) + 0.8 * ring;
			Point p{bpos.x + std::cos(angle) * r, bpos.y + std::sin(angle) * r};
			auto tile = phys(p).to_tile();
			if (grid and not map->is_passable(*grid, tile)) {
				continue;
			}
			spot = p;
			found = true;
		}
	}
	auto pos = component_of<component::Position>(unit.entity, component::component_t::POSITION);
	if (pos) {
		pos->set_position(time, map->on_terrain(phys(spot)));
	}
	unit.post = spot;
	auto activity = component_of<component::Activity>(unit.entity, component::component_t::ACTIVITY);
	if (activity) {
		activity->cancel_events(time);
		activity->init(time);
		unit.entity->get_manager()->run_activity_system(time);
	}
	log::log(INFO << "Combat: " << describe(unit.entity, *unit.stats, this->owner_of(unit.entity, time))
	              << " leaves " << describe(building.entity, *building.stats, this->owner_of(building.entity, time))
	              << (unit.by_bell ? " (back to work)" : ""));
	if (unit.by_bell) {
		this->resume_job(state, unit, time);
	}
	unit.by_bell = false;
	this->publish_info(unit);
	this->publish_info(building);
	this->publish_hidden();
}

void CombatState::eject_all(const std::shared_ptr<GameState> &state, Combatant &building, bool bell_only,
                            const time::time_t &time) {
	auto ids = building.occupants;
	size_t slot = 0;
	for (auto id : ids) {
		auto *u = this->find(id);
		if (u == nullptr) {
			std::erase(building.occupants, id);
			continue;
		}
		if (bell_only and not u->by_bell) {
			continue;
		}
		this->leave(state, *u, building, slot++, time);
	}
}

void CombatState::ungarrison_all(const std::shared_ptr<GameState> &state, entity_id_t building, const time::time_t &time) {
	auto *b = this->find(building);
	if (b == nullptr or b->occupants.empty()) {
		return;
	}
	log::log(INFO << "Combat: " << describe(b->entity, *b->stats, this->owner_of(b->entity, time)) << " lets "
	              << b->occupants.size() << " units out");
	this->eject_all(state, *b, false, time);
}

double CombatState::running_arrows(const Combatant &building, const time::time_t &time) const {
	if (not building.stats->shoots_arrows()) {
		return 0.0;
	}
	double running = static_cast<double>(building.stats->min_projectiles);
	if (building.occupants.empty()) {
		return running;
	}
	const double building_dps = building.stats->dps();
	// infantry counts only with crenellations (a technology of the data, 05 §5.4)
	const bool crenellations = this->crenellations_players.contains(this->owner_of(building.entity, time));
	for (auto id : building.occupants) {
		const auto *u = this->find(id);
		if (u == nullptr) {
			continue;
		}
		// ranged units: attack of their projectile per reload time
		const double unit_dps = u->stats->dps();
		running += arrow_contribution(u->stats->unit_class, u->stats->villager, unit_dps, building_dps, crenellations);
	}
	return running;
}

bool CombatState::ring_bell(const std::shared_ptr<GameState> &state, entity_id_t tc, bool on, const time::time_t &time) {
	auto *t = this->find(tc);
	if (t == nullptr or t->dead or not t->stats->town_center) {
		return false;
	}
	const auto owner = this->owner_of(t->entity, time);
	const auto center = ground(this->position_of(t->entity, time));
	// shelters in the area: own finished town centers, towers, castles (06 §6.4)
	std::vector<Combatant *> shelters;
	for (auto &[id, c] : this->combatants) {
		if (c.dead or not c.stats->building or c.stats->garrison_capacity <= 0 or not this->finished(c)
		    or this->owner_of(c.entity, time) != owner) {
			continue;
		}
		if (not(c.stats->town_center or c.stats->unit_class == CLASS_TOWER or c.stats->name == "Castle")) {
			continue;
		}
		if (in_bell_area(center, ground(this->position_of(c.entity, time)))) {
			shelters.push_back(&c);
		}
	}
	std::sort(shelters.begin(), shelters.end(), [](const auto *a, const auto *b) {
		return a->entity->get_id() < b->entity->get_id();
	});
	if (on) {
		std::vector<BellVillager> villagers;
		for (auto &[id, c] : this->combatants) {
			if (c.dead or not c.stats->villager or c.inside or c.entering or this->owner_of(c.entity, time) != owner) {
				continue;
			}
			const auto p = ground(this->position_of(c.entity, time));
			if (in_bell_area(center, p)) {
				villagers.push_back({id, p});
			}
		}
		std::sort(villagers.begin(), villagers.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
		std::vector<BellShelter> places;
		for (auto *s : shelters) {
			int pending = 0;
			for (const auto &[id, c] : this->combatants) {
				pending += c.entering and *c.entering == s->entity->get_id() ? 1 : 0;
			}
			const int free = s->stats->garrison_capacity - static_cast<int>(s->occupants.size()) - pending;
			places.push_back({s->entity->get_id(), ground(this->position_of(s->entity, time)), free});
			s->bell = true;
			this->publish_info(*s);
		}
		t->bell = true;
		size_t sent = 0;
		for (const auto &[v, s] : assign_bell(villagers, places)) {
			sent += this->order_garrison(state, v, s, time, true) ? 1 : 0;
		}
		this->publish_info(*t);
		log::log(INFO << "Combat: town bell of " << describe(t->entity, *t->stats, owner) << ": " << sent << " of "
		              << villagers.size() << " villagers go into " << shelters.size() << " buildings at t="
		              << time.to_double());
		return true;
	}
	// back to work: the bell occupants come out, villagers on their way turn back
	size_t out = 0;
	for (auto *s : shelters) {
		for (auto id : s->occupants) {
			const auto *u = this->find(id);
			out += u and u->by_bell ? 1 : 0;
		}
		this->eject_all(state, *s, true, time);
		s->bell = false;
		this->publish_info(*s);
	}
	for (auto &[id, c] : this->combatants) {
		if (c.entering and c.by_bell and this->owner_of(c.entity, time) == owner) {
			c.entering.reset();
			this->resume_job(state, c, time);
			c.by_bell = false;
			this->publish_info(c);
		}
	}
	t->bell = false;
	this->publish_info(*t);
	log::log(INFO << "Combat: town bell of " << describe(t->entity, *t->stats, owner) << " off: " << out
	              << " villagers back to work at t=" << time.to_double());
	return true;
}

bool CombatState::bell_rings(entity_id_t tc) const {
	const auto *t = this->find(tc);
	return t and t->bell;
}

void CombatState::remember_job(Combatant &c) {
	if (c.resume_gather or c.resume_build) {
		return;
	}
	if (auto gather = component_of<component::Gather>(c.entity, component::component_t::GATHER)) {
		const auto &job = gather->get_job();
		if (job.phase != component::Gather::phase_t::NONE and job.target) {
			c.resume_gather = *job.target;
		}
	}
	if (auto builder = component_of<component::Builder>(c.entity, component::component_t::BUILDER)) {
		const auto &job = builder->get_job();
		if (job.phase != component::Builder::phase_t::NONE and job.target) {
			c.resume_build = *job.target;
		}
	}
}

void CombatState::resume_job(const std::shared_ptr<GameState> &state, Combatant &c, const time::time_t &time) {
	auto queue = component_of<component::CommandQueue>(c.entity, component::component_t::COMMANDQUEUE);
	const auto &entities = state->get_game_entities();
	if (queue) {
		if (c.resume_build and entities.contains(*c.resume_build)) {
			queue->add_command(time, std::make_shared<component::command::BuildCommand>(*c.resume_build));
		}
		else if (c.resume_gather and entities.contains(*c.resume_gather)) {
			queue->add_command(time, std::make_shared<component::command::GatherCommand>(*c.resume_gather));
		}
		else if (c.by_bell) {
			queue->add_command(time, std::make_shared<component::command::MoveCommand>(
										 state->get_map()->on_terrain(phys(c.bell_from))));
		}
	}
	c.resume_build.reset();
	c.resume_gather.reset();
}

std::shared_ptr<GameEntity> CombatState::pick_shelter(const std::shared_ptr<GameState> &state,
                                                      const time::time_t &time,
                                                      player_id_t player,
                                                      const PickRequest &request) const {
	return pick_entity(state, time, request, [&](const std::shared_ptr<GameEntity> &entity) -> std::optional<PickShape> {
		auto c = this->find(entity->get_id());
		if (not c or c->dead or not c->stats->building or c->stats->garrison_capacity <= 0
		    or this->owner_of(entity, time) != player) {
			return std::nullopt;
		}
		return PickShape{c->stats->radius, c->stats->radius};
	});
}

std::shared_ptr<GameEntity> CombatState::pick_unit(const std::shared_ptr<GameState> &state,
                                                   const time::time_t &time,
                                                   const PickRequest &request,
                                                   entity_id_t except) const {
	return pick_entity(state, time, request, [&](const std::shared_ptr<GameEntity> &entity) -> std::optional<PickShape> {
		auto c = this->find(entity->get_id());
		if (not c or c->dead or c->inside or entity->get_id() == except or not c->stats->attackable()
		    or this->neutral.contains(this->owner_of(entity, time))) {
			return std::nullopt;
		}
		return PickShape{c->stats->radius, c->stats->building ? c->stats->radius : 1.0};
	});
}

std::shared_ptr<GameEntity> CombatState::pick_enemy(const std::shared_ptr<GameState> &state,
                                                    const time::time_t &time,
                                                    player_id_t player,
                                                    const PickRequest &request) const {
	return pick_entity(state, time, request, [&](const std::shared_ptr<GameEntity> &entity) -> std::optional<PickShape> {
		auto c = this->find(entity->get_id());
		if (not c or c->dead or c->inside or not c->stats->attackable()) {
			return std::nullopt;
		}
		auto owner = this->owner_of(entity, time);
		if (owner == player or this->neutral.contains(owner)) {
			return std::nullopt;
		}
		// XR fork (vision): enemies in the fog cannot be picked, remembered buildings can
		if (this->vision) {
			auto pos = this->position_of(entity, time);
			if (not this->vision->knows(player, entity->get_id(), pos.ne.to_double(), pos.se.to_double(),
			                            c->stats->building ? c->stats->radius : 0.0, c->stats->building)) {
				return std::nullopt;
			}
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
	if (not attacker.post and attacker.stats->movable and not attacker.inside) {
		attacker.post = ground(this->position_of(attacker.entity, time));
	}
	attacker.target = target;
	attacker.explicit_order = explicit_order;
	attacker.chase_destination.reset();
	attacker.chase_failures = 0;
	if (explicit_order) {
		attacker.order = order_t::ATTACK;
	}
	else if (attacker.stance == stance_t::DEFENSIVE or attacker.stance == stance_t::STAND_GROUND) {
		attacker.return_to_post = true;
	}
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
	if (attacker.order == order_t::ATTACK) {
		attacker.order = order_t::NONE;
		this->publish_info(attacker);
	}
	if (attacker.tick) {
		attacker.tick->cancel(time);
		attacker.tick = nullptr;
	}
	if (not attacker.dead and not attacker.inside and not this->is_moving(attacker.entity, time)
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

// ---------------------------------------------------------------- elevation and terrain (05 §5.2)

double CombatState::ground_height(const std::shared_ptr<GameState> &state, double ne, double se) const {
	const auto &heights = state->get_map()->get_heightmap();
	return heights.is_flat() ? 0.0 : heights.at(ne, se);
}

double CombatState::elevation_between(const std::shared_ptr<GameState> &state, const Point3 &a, const Point3 &b,
                                      double high) const {
	const auto &heights = state->get_map()->get_heightmap();
	if (heights.is_flat()) {
		return 1.0;
	}
	const double ha = heights.at(a.ne, a.se);
	const double hb = heights.at(b.ne, b.se);
	// samples of the line in half tile steps (without the end points)
	const double d = std::hypot(b.ne - a.ne, b.se - a.se);
	const int steps = static_cast<int>(std::floor(d / 0.5));
	double line_max = -1e30;
	for (int i = 1; i < steps; ++i) {
		const double s = static_cast<double>(i) * 0.5 / d;
		line_max = std::max(line_max, heights.at(a.ne + (b.ne - a.ne) * s, a.se + (b.se - a.se) * s));
	}
	return elevation_factor(ha, hb, line_max, high, ELEVATION_LOW);
}

double CombatState::terrain_factor_of(const std::shared_ptr<GameState> &state, const Combatant &target,
                                      const time::time_t &time) {
	if (target.stats->defense_terrain < 0) {
		return 1.0;
	}
	if (not this->terrain_loaded) {
		this->terrain_loaded = true;
		try {
			// global table of the data (engine.xr, W0): restriction factors and terrain ids
			auto view = state->get_player(this->owner_of(target.entity, time))->get_db_view();
			nyan::Object data = view->get_object(this->modpack + ".data.util.xr.genie.GenieData");
			for (const auto &value : data.get_orderedset("GenieData.terrain_restrictions")) {
				auto obj_value = std::dynamic_pointer_cast<nyan::ObjectValue>(value.get_ptr());
				if (not obj_value) {
					continue;
				}
				auto restriction = view->get_object(obj_value->get_name());
				auto id = static_cast<int>(restriction.get_int("GenieTerrainRestriction.restriction_id"));
				auto &factors = this->terrain_restrictions[id];
				for (const auto &[k, v] : restriction.get_dict("GenieTerrainRestriction.factors")) {
					auto key = std::dynamic_pointer_cast<nyan::Int>(k.get_ptr());
					auto fl = std::dynamic_pointer_cast<nyan::Float>(v.get_ptr());
					if (key and fl) {
						factors[static_cast<int>(key->get())] = fl->get();
					}
				}
			}
			for (const auto &[k, v] : data.get_dict("GenieData.terrains")) {
				auto key = std::dynamic_pointer_cast<nyan::Int>(k.get_ptr());
				auto obj = std::dynamic_pointer_cast<nyan::ObjectValue>(v.get_ptr());
				if (key and obj) {
					this->terrain_ids[obj->get_name()] = static_cast<int>(key->get());
				}
			}
			log::log(INFO << "Combat: terrain factors of " << this->terrain_restrictions.size() << " restrictions, "
			              << this->terrain_ids.size() << " terrains");
		}
		catch (std::exception &err) {
			log::log(INFO << "Combat: no terrain factors in the data (" << err.what() << ")");
		}
	}
	auto restriction = this->terrain_restrictions.find(target.stats->defense_terrain);
	if (restriction == this->terrain_restrictions.end()) {
		return 1.0;
	}
	const auto name = state->get_map()->terrain_name(this->position_of(target.entity, time).to_tile());
	auto id = this->terrain_ids.find(name);
	if (id == this->terrain_ids.end()) {
		return 1.0;
	}
	auto factor = restriction->second.find(id->second);
	return terrain_factor(factor == restriction->second.end() ? std::nullopt : std::optional<double>{factor->second});
}

// ---------------------------------------------------------------- event handlers

void CombatState::tick(const std::shared_ptr<GameState> &state, entity_id_t attacker_id, const time::time_t &time) {
	auto a = this->find(attacker_id);
	if (not a or a->dead or a->inside) {
		return;
	}
	a->tick = nullptr;

	// XR fork W4: on the way into a building
	if (a->entering) {
		auto *b = this->find(*a->entering);
		if (b == nullptr or not this->accepts(*b, *a, time)) {
			log::log(INFO << "Combat: " << describe(a->entity, *a->stats, this->owner_of(a->entity, time))
			              << " cannot garrison (building gone or full)");
			a->entering.reset();
			if (a->by_bell) {
				this->resume_job(state, *a, time);
				a->by_bell = false;
			}
			this->publish_info(*a);
			return;
		}
		const double edge = this->edge_between(*a, *b, time);
		if (edge <= ENTER_DISTANCE) {
			this->enter(state, *a, *b, time);
			return;
		}
		const bool moving = this->is_moving(a->entity, time);
		if (not moving) {
			auto apos = ground(this->position_of(a->entity, time));
			Point dest = approach_point(apos, a->stats->radius, footprint(*b->stats, this->position_of(b->entity, time)),
			                            0.2, 0.785398 * a->chase_failures);
			if (a->chase_destination and time - a->chase_ordered > seconds(0.5)) {
				a->chase_failures += 1;
			}
			if (a->chase_failures > 2 * MAX_CHASE_FAILURES) {
				log::log(INFO << "Combat: " << describe(a->entity, *a->stats, this->owner_of(a->entity, time))
				              << " cannot reach the building to garrison");
				a->entering.reset();
				a->chase_failures = 0;
				this->publish_info(*a);
				return;
			}
			auto queue = component_of<component::CommandQueue>(a->entity, component::component_t::COMMANDQUEUE);
			auto target_pos = state->get_map()->on_terrain(phys(dest));
			queue->add_command(time, std::make_shared<component::command::MoveCommand>(target_pos));
			a->chase_destination = target_pos;
			a->chase_ordered = time;
		}
		this->schedule_tick(state, *a, time + seconds(CHASE_PERIOD));
		return;
	}

	if (not a->target) {
		return;
	}

	auto t = this->find(*a->target);
	if (not t or not this->is_enemy(*a, *t, time)) {
		// target died or changed sides: continue with the next enemy in sight (not villagers)
		a->target.reset();
		a->explicit_order = false;
		if (a->order == order_t::ATTACK) {
			a->order = order_t::NONE;
			this->publish_info(*a);
		}
		if (not a->stats->villager) {
			auto next = this->choose_target(*a, time);
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
		if (a->stats->building and arrow_count(this->running_arrows(*a, time), a->stats->max_projectiles) <= 0) {
			// no base arrow and nobody inside: the building cannot shoot
			this->drop_target(*a, time);
			return;
		}
		if (time >= a->ready_at) {
			a->ready_at = time + seconds(std::max(0.1, a->stats->reload_time));
			if (not a->stats->attack_animation.empty()) {
				a->entity->render_update(time, a->stats->attack_animation);
			}
			this->attack(state, *a, *t, time);
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
	// XR fork W4: stances (06 §6.2) – stand ground does not chase, defensive gives up far from the post
	if (not a->explicit_order) {
		if (not stance_moves(a->stance) or a->stance == stance_t::NO_ATTACK) {
			this->drop_target(*a, time);
			return;
		}
		if (stance_returns(a->stance, this->from_post(*a, ground(apos), time)) and a->order != order_t::ATTACK_MOVE) {
			this->drop_target(*a, time);
			if (a->post) {
				this->order_move(state, *a, *a->post, time, true);
			}
			return;
		}
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
			// XR fork W4: the failed target is locked for 10 s (06 §6.2)
			a->locks[t->entity->get_id()] = time.to_double() + TARGET_LOCK;
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

void CombatState::attack(const std::shared_ptr<GameState> &state, Combatant &attacker, Combatant &target,
                         const time::time_t &time) {
	if (attacker.stats->ranged) {
		this->launch(state, attacker, target, time);
		return;
	}
	// melee (05 §5.2): hit roll with the accuracy (100 for most), elevation and terrain factor
	std::uniform_real_distribution<double> roll{0.0, 100.0};
	if (not roll_hit(roll(this->rng), attacker.stats->accuracy)) {
		return;
	}
	auto apos = this->position_of(attacker.entity, time);
	auto tpos = this->position_of(target.entity, time);
	const double factor = this->elevation_between(state, {apos.ne.to_double(), apos.se.to_double(), 0.0},
	                                              {tpos.ne.to_double(), tpos.se.to_double(), 0.0},
	                                              attacker.stats->elevation_high)
	                      * this->terrain_factor_of(state, target, time);
	this->apply_hit(state, *attacker.stats, attacker.entity->get_id(), this->owner_of(attacker.entity, time), target,
	                factor, time);
}

void CombatState::launch(const std::shared_ptr<GameState> &state, Combatant &attacker, Combatant &target,
                         const time::time_t &time) {
	const auto &s = *attacker.stats;
	const int count = s.building ? arrow_count(this->running_arrows(attacker, time), s.max_projectiles)
	                             : std::max(1, s.min_projectiles);
	if (count <= 0) {
		return;
	}
	std::uniform_real_distribution<double> u01{0.0, 1.0};
	std::uniform_real_distribution<double> roll{0.0, 100.0};
	const auto owner = this->owner_of(attacker.entity, time);
	const auto apos = this->position_of(attacker.entity, time);
	const auto tpos = this->position_of(target.entity, time);
	const Point shooter = ground(apos);
	Point aim = ground(tpos);
	// lead (ballistics: the projectile aims where a moving target will be, 05 §5.3)
	if (s.projectile.lead and target.stats->movable) {
		const auto later = ground(this->position_of(target.entity, time + seconds(0.5)));
		const Point velocity{(later.x - aim.x) / 0.5, (later.y - aim.y) / 0.5};
		aim = lead_point(shooter, aim, velocity, s.projectile.speed);
	}
	const double d = distance(shooter, aim);
	const double launch_height = this->ground_height(state, shooter.x, shooter.y)
	                             + (s.building ? std::clamp(s.spawn_height * 0.5, 1.0, 3.0) : 0.9);
	const Point3 from{shooter.x, shooter.y, launch_height};
	const double elevation = this->elevation_between(state, {shooter.x, shooter.y, 0.0}, {aim.x, aim.y, 0.0}, s.elevation_high);
	const double now = time.to_double();

	for (int i = 0; i < count; ++i) {
		Projectile p;
		p.id = this->next_projectile++;
		p.shooter = attacker.entity->get_id();
		p.owner = owner;
		p.stats = attacker.stats;
		p.elevation = elevation;
		p.secondary = i > 0;
		const auto &ps = (i > 0 and s.has_secondary) ? s.secondary : s.projectile;
		Point point = aim;
		if (i == 0) {
			// the first one is a normal shot with the hit roll (05 §5.3)
			if (roll_hit(roll(this->rng), ps.accuracy)) {
				p.target = target.entity->get_id();
			}
			else {
				const double r1 = u01(this->rng);
				const double r2 = u01(this->rng);
				point = miss_point(aim, ps.dispersion, d, r1, r2);
			}
		}
		else {
			// extra garrison arrows: random on the spawning area around the aim (05 §5.4)
			const double r1 = u01(this->rng);
			const double r2 = u01(this->rng);
			point = area_point(aim, aim.x - shooter.x, aim.y - shooter.y, std::max(0.5, s.spawn_width),
			                   std::max(0.5, s.spawn_length), r1, r2);
			this->projectile_count.extra_arrows += 1;
		}
		const double target_ground = this->ground_height(state, point.x, point.y);
		const Point3 to{point.x, point.y, aim_height(target_ground, p.target ? target.stats->height : 0.0)};
		p.flight = plan_flight(from, to, ps.speed, ps.arc);
		// groups of 1-2 arrows every 0.05 s
		p.launched = now + 0.05 * static_cast<double>((i + 1) / 2);
		openage::event::EventHandler::param_map::map_t params{{"projectile", p.id}};
		this->loop->create_event("game.combat_projectile",
		                         std::static_pointer_cast<CombatState>(state->get_combat()),
		                         state,
		                         seconds(p.launched + p.flight.duration),
		                         params);
		this->projectile_count.launched += 1;
		if (i == 0 and (this->allow_hit_log(time) or not attacker.hit_logged)) {
			attacker.hit_logged = true;
			log::log(INFO << "Combat: " << describe(attacker.entity, s, owner) << " shoots at "
			              << describe(target.entity, *target.stats, this->owner_of(target.entity, time))
			              << (p.target ? " (hit roll ok)" : " (miss)") << ", " << count << " projectile(s), flight "
			              << p.flight.duration << " s, arc peak " << p.flight.peak << ", elevation x" << elevation
			              << " (t=" << now << ")");
		}
		this->projectiles.emplace(p.id, std::move(p));
	}
	this->publish_projectiles();
}

void CombatState::impact(const std::shared_ptr<GameState> &state, uint64_t projectile, const time::time_t &time) {
	auto it = this->projectiles.find(projectile);
	if (it == this->projectiles.end()) {
		return;
	}
	Projectile p = std::move(it->second);
	this->projectiles.erase(it);
	if (p.target) {
		auto *t = this->find(*p.target);
		if (t != nullptr and not t->dead and not t->inside) {
			this->projectile_count.hits += 1;
			this->apply_hit(state, *p.stats, p.shooter, p.owner, *t, p.elevation * this->terrain_factor_of(state, *t, time), time);
			this->publish_projectiles();
			return;
		}
	}
	// missed (or the target is gone): the first object it touches that is neither the shooter's
	// nor gaia's (05 §5.3), on the low last part of the flight
	this->projectile_count.misses += 1;
	const auto a3 = flight_at(p.flight, p.flight.duration * (1.0 - MISS_TOUCH_SHARE));
	const Point a{a3.ne, a3.se};
	const Point b{p.flight.to.ne, p.flight.to.se};
	Combatant *best = nullptr;
	double best_s = 2.0;
	for (auto &[id, c] : this->combatants) {
		if (c.dead or c.inside or not c.stats->attackable() or id == p.shooter) {
			continue;
		}
		const auto owner = this->owner_of(c.entity, time);
		if (owner == p.owner or this->neutral.contains(owner)) {
			continue;
		}
		auto touch = segment_touches(a, b, footprint(*c.stats, this->position_of(c.entity, time)));
		if (touch and (*touch < best_s or (*touch == best_s and best and id < best->entity->get_id()))) {
			best_s = *touch;
			best = &c;
		}
	}
	if (best != nullptr) {
		this->projectile_count.miss_hits += 1;
		this->apply_hit(state, *p.stats, p.shooter, p.owner, *best, p.elevation * this->terrain_factor_of(state, *best, time),
		                time);
	}
	this->publish_projectiles();
}

void CombatState::apply_hit(const std::shared_ptr<GameState> &state,
                            const CombatStats &attacker_stats,
                            entity_id_t attacker_id,
                            player_id_t attacker_owner,
                            Combatant &target,
                            double factor,
                            const time::time_t &time) {
	auto live = std::dynamic_pointer_cast<component::Live>(
		target.entity->get_component(component::component_t::LIVE));
	auto current = live->get_attribute(time, target.stats->health_attribute);
	if (not current or target.dead) {
		return;
	}
	auto damage = compute_damage(attacker_stats.attack, target.stats->armor, factor);
	auto health = std::max<int64_t>(0, *current - damage);
	live->set_attribute(time, target.stats->health_attribute, health);
	this->set_health_snapshot(target.entity->get_id(), health, target.stats->max_health, health > 0);

	auto target_owner = this->owner_of(target.entity, time);
	if (this->allow_hit_log(time)) {
		log::log(INFO << "Combat: " << describe(attacker_id, attacker_stats, attacker_owner)
		              << " hits " << describe(target.entity, *target.stats, target_owner)
		              << " for " << damage << (factor != 1.0 ? " (x" + std::to_string(factor).substr(0, 4) + ")" : std::string{})
		              << ", health " << health << "/" << target.stats->max_health
		              << " (t=" << time.to_double() << ")");
	}
	// XR fork W4: threat memory of the victim's owner (06 §6.2)
	this->threats[target_owner].add(time.to_double(),
	                                category_of(attacker_stats.unit_class, attacker_stats.building, attacker_stats.villager));

	if (health <= 0) {
		this->die(state, target, time, describe(attacker_id, attacker_stats, attacker_owner));
		return;
	}

	// buildings under 20 % throw their garrison out (06 §6.5)
	if (target.stats->building and not target.occupants.empty()
	    and static_cast<double>(health) < EJECT_HEALTH * static_cast<double>(target.stats->max_health)) {
		log::log(INFO << "Combat: " << describe(target.entity, *target.stats, target_owner)
		              << " below 20 % health, the garrison comes out");
		this->eject_all(state, target, false, time);
	}

	// villagers flee (06 §6.4)
	if (target.stats->villager) {
		this->attacked(state, target, attacker_id, attacker_owner, time);
		return;
	}

	// fight back (military units without a target; not against buildings, 06 §6.2)
	auto *attacker = this->find(attacker_id);
	if (attacker == nullptr or attacker->dead or attacker->inside or attacker->stats->building) {
		return;
	}
	if (not target.target and target.stats->can_attack and not target.stats->villager and target.order != order_t::FOLLOW
	    and (target.stats->movable or attacker_stats.ranged or target.stats->ranged)) {
		if (target.stats->building) {
			this->engage(state, target, attacker_id, false, time);
			return;
		}
		if (target.stance == stance_t::NO_ATTACK) {
			return;
		}
		if (target.stance == stance_t::STAND_GROUND
		    and not in_attack_range(this->edge_between(target, *attacker, time), target.stats->min_range,
		                            target.stats->max_range)) {
			return;
		}
		this->engage(state, target, attacker_id, false, time);
	}
}

void CombatState::attacked(const std::shared_ptr<GameState> &state, Combatant &victim, entity_id_t attacker,
                           player_id_t attacker_owner, const time::time_t &time) {
	if (victim.inside or victim.entering or this->neutral.contains(attacker_owner)) {
		// gaia (wolves, boars): the economy decides (W3), no flight
		return;
	}
	if (victim.target and victim.explicit_order) {
		return;  // ordered to attack: fights
	}
	const double now = time.to_double();
	std::erase_if(victim.attackers, [&](const auto &a) { return a.first == attacker or now - a.second > FLEE_CALM; });
	victim.attackers.emplace_back(attacker, now);
	if (victim.fleeing and now - victim.flee_ordered < 1.0) {
		return;
	}
	std::vector<Point> from;
	for (const auto &[id, t] : victim.attackers) {
		if (auto *a = this->find(id); a and not a->dead) {
			from.push_back(ground(this->position_of(a->entity, time)));
		}
	}
	const auto pos = ground(this->position_of(victim.entity, time));
	std::uniform_real_distribution<double> angle{0.0, 6.283185307};
	std::optional<Point> post;
	if (victim.stance == stance_t::DEFENSIVE and victim.post) {
		post = victim.post;
	}
	const Point dest = flee_point(pos, from, victim.stats->speed, angle(this->rng), post);
	if (not victim.fleeing) {
		this->remember_job(victim);
		log::log(INFO << "Combat: " << describe(victim.entity, *victim.stats, this->owner_of(victim.entity, time))
		              << " flees from " << from.size() << " attacker(s) at t=" << now);
	}
	victim.fleeing = true;
	victim.flee_ordered = now;
	auto queue = component_of<component::CommandQueue>(victim.entity, component::component_t::COMMANDQUEUE);
	if (queue) {
		queue->add_command(time, std::make_shared<component::command::MoveCommand>(state->get_map()->on_terrain(phys(dest))));
	}
}

void CombatState::update_flight(const std::shared_ptr<GameState> &state, Combatant &c, const time::time_t &time) {
	if (not c.fleeing or c.dead) {
		return;
	}
	const double now = time.to_double();
	double last = -1e9;
	for (const auto &[id, t] : c.attackers) {
		last = std::max(last, t);
	}
	if (now - last >= FLEE_CALM and not this->is_moving(c.entity, time)) {
		c.fleeing = false;
		c.attackers.clear();
		log::log(INFO << "Combat: " << describe(c.entity, *c.stats, this->owner_of(c.entity, time))
		              << " calm again, back to work at t=" << now);
		this->resume_job(state, c, time);
	}
}

void CombatState::die(const std::shared_ptr<GameState> &state,
                      Combatant &victim,
                      const time::time_t &time,
                      const std::string &cause) {
	if (victim.dead) {
		return;
	}
	// XR fork W4: the garrison survives a destroyed building
	if (not victim.occupants.empty()) {
		this->eject_all(state, victim, false, time);
	}
	victim.dead = true;
	victim.target.reset();
	victim.entering.reset();
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
	if (c->inside) {
		// a garrisoned unit leaves first (it is shown dying next to the building)
		if (auto *b = this->find(*c->inside)) {
			this->leave(state, *c, *b, 0, time);
		}
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
		this->stats_snapshot.erase(id);
		this->info_snapshot.erase(id);
	}
	log::log(DBG << "Combat: entity " << id << " removed at t=" << time.to_double());
}

void CombatState::scan(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	this->update_match(time);
	const double now = time.to_double();
	const double dt = std::clamp(now - this->last_scan, 0.0, 5.0);
	this->last_scan = now;
	for (auto &[player, memory] : this->threats) {
		memory.prune(now);
	}
	// infantry adds garrison arrows only with crenellations (05 §5.4, a technology of the data)
	this->crenellations_players.clear();
	this->herbal_players.clear();
	if (auto tech = state->get_tech()) {
		for (auto player : tech->players()) {
			if (const auto *t = tech->find(player)) {
				for (const auto &fqon : t->order) {
					if (fqon.size() >= 13 and fqon.compare(fqon.size() - 13, 13, ".Crenellations") == 0) {
						this->crenellations_players.insert(player);
					}
					if (fqon.size() >= 15 and fqon.compare(fqon.size() - 15, 15, ".HerbalMedicine") == 0) {
						this->herbal_players.insert(player);
					}
				}
			}
		}
	}

	std::vector<entity_id_t> ids;
	ids.reserve(this->combatants.size());
	for (const auto &[id, c] : this->combatants) {
		if (not c.dead) {
			ids.push_back(id);
		}
	}
	std::sort(ids.begin(), ids.end());

	// XR fork W4: healing in the garrison, orders, flight, stance leash
	for (auto id : ids) {
		auto *c = this->find(id);
		if (c == nullptr or c->dead) {
			continue;
		}
		if (c->stats->building and not c->occupants.empty() and c->stats->garrison_heal > 0.0) {
			const double rate = c->stats->garrison_heal
			                    * (this->herbal_players.contains(this->owner_of(c->entity, time)) ? HERBAL_MEDICINE : 1.0);
			for (auto uid : c->occupants) {
				auto *u = this->find(uid);
				if (u == nullptr) {
					continue;
				}
				auto live = component_of<component::Live>(u->entity, component::component_t::LIVE);
				auto current = live ? live->get_attribute(time, u->stats->health_attribute) : std::nullopt;
				if (not current) {
					continue;
				}
				auto add = garrison_heal(rate, dt, u->heal_carry, u->stats->max_health - *current);
				if (add > 0) {
					live->set_attribute(time, u->stats->health_attribute, *current + add);
					this->set_health_snapshot(uid, *current + add, u->stats->max_health, true);
				}
			}
		}
		// units without a command keep the place where they stand as their post
		if (not c->post and c->stats->movable and not c->inside and not this->is_moving(c->entity, time)) {
			c->post = ground(this->position_of(c->entity, time));
		}
		this->update_flight(state, *c, time);
		this->update_order(state, *c, time);
	}

	for (auto id : ids) {
		auto c = this->find(id);
		// units attacking a building look for a better target (06 §6.2: typically an attacking unit)
		if (c and not c->dead and c->target and c->stats->movable and not c->stats->villager) {
			const auto *t = this->find(*c->target);
			if (t and t->stats->building) {
				if (auto better = this->choose_target(*c, time)) {
					log::log(INFO << "Combat: " << describe(c->entity, *c->stats, this->owner_of(c->entity, time))
					              << " leaves the building for a unit");
					this->engage(state, *c, *better, false, time);
				}
			}
			continue;
		}
		if (not c or c->dead or c->target or c->inside or c->entering or not c->stats->can_attack or c->stats->villager) {
			continue;
		}
		auto owner = this->owner_of(c->entity, time);
		if (this->neutral.contains(owner)) {
			continue;
		}
		if (c->stats->movable) {
			// walking units only look around on patrol, attack move or guard
			const bool searching = c->order == order_t::PATROL or c->order == order_t::ATTACK_MOVE
			                       or c->order == order_t::GUARD;
			if (this->is_moving(c->entity, time) and not searching) {
				continue;
			}
		}
		else if (c->stats->building and arrow_count(this->running_arrows(*c, time), c->stats->max_projectiles) <= 0) {
			continue;
		}
		auto enemy = this->choose_target(*c, time);
		if (enemy) {
			auto e = this->find(*enemy);
			if (this->allow_hit_log(time)) {
				log::log(INFO << "Combat: auto attack " << describe(c->entity, *c->stats, owner)
				              << " -> " << describe(e->entity, *e->stats, this->owner_of(e->entity, time))
				              << " (" << to_string(c->stance) << ")");
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
	std::map<player_id_t, size_t> units;
	for (auto player : this->participants) {
		alive[player] = 0;
		units[player] = 0;
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
		if (c.stats->unit and not c.stats->building) {
			units[owner] += 1;
		}
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
		this->match_status.units = units;
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

// ---------------------------------------------------------------- save games (XR fork W4)

std::optional<SavedCombat> CombatState::save_entity(entity_id_t id) const {
	const auto *c = this->find(id);
	if (c == nullptr or c->dead) {
		return std::nullopt;
	}
	SavedCombat s;
	s.stance = c->stance;
	s.formation = c->formation;
	if (c->order == order_t::PATROL or c->order == order_t::ATTACK_MOVE or c->order == order_t::GUARD
	    or c->order == order_t::FOLLOW) {
		s.order = c->order;
		s.route = c->route;
		s.order_entity = c->order_entity;
	}
	s.inside = c->inside;
	s.by_bell = c->by_bell;
	s.bell_from = c->bell_from;
	s.resume_gather = c->resume_gather;
	s.resume_build = c->resume_build;
	s.bell = c->bell;
	if (s.stance == stance_t::AGGRESSIVE and s.formation == formation_t::NONE and s.order == order_t::NONE
	    and not s.inside and not s.bell) {
		return std::nullopt;
	}
	return s;
}

void CombatState::load_entity(const std::shared_ptr<GameState> &state, entity_id_t id, const SavedCombat &saved,
                              const time::time_t &time) {
	auto *c = this->find(id);
	if (c == nullptr or c->dead) {
		return;
	}
	if (c->stats->stances) {
		c->stance = saved.stance;
	}
	if (c->stats->formations) {
		c->formation = saved.formation;
	}
	c->bell = saved.bell;
	if (saved.inside) {
		if (auto *b = this->find(*saved.inside); b and this->accepts(*b, *c, time)) {
			c->by_bell = saved.by_bell;
			c->bell_from = saved.bell_from;
			c->resume_gather = saved.resume_gather;
			c->resume_build = saved.resume_build;
			this->enter(state, *c, *b, time);
		}
		else {
			log::log(INFO << "Load: garrison building " << *saved.inside << " of entity " << id << " is gone");
		}
	}
	else if (saved.order != order_t::NONE) {
		c->order = saved.order;
		c->route = saved.route;
		c->leg = c->route.size() >= 2 ? 1 : 0;
		c->order_entity = saved.order_entity;
	}
	this->publish_info(*c);
}

// ---------------------------------------------------------------- snapshots

void CombatState::publish_info(const Combatant &c) {
	CombatInfo info;
	info.stance = c.stance;
	info.order = c.order;
	info.formation = c.formation;
	info.stances = c.stats->stances and c.stats->military();
	info.formations = c.stats->formations and c.stats->military();
	info.inside = c.inside.has_value();
	info.capacity = c.stats->garrison_capacity;
	for (auto id : c.occupants) {
		if (const auto *u = this->find(id)) {
			info.occupants.push_back(u->stats->name);
		}
	}
	if (c.stats->shoots_arrows()) {
		info.arrows = arrow_count(this->running_arrows(c, this->clock), c.stats->max_projectiles);
	}
	info.bell = c.bell;
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	this->info_snapshot[c.entity->get_id()] = std::move(info);
}

void CombatState::publish_hidden() {
	auto hidden = std::make_shared<std::unordered_set<entity_id_t>>();
	for (const auto &[id, c] : this->combatants) {
		if (c.inside) {
			hidden->insert(id);
		}
	}
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	this->hidden_snapshot = std::move(hidden);
	this->hidden_version += 1;
}

void CombatState::publish_projectiles() {
	std::vector<ProjectileView> views;
	views.reserve(this->projectiles.size());
	for (const auto &[id, p] : this->projectiles) {
		views.push_back({p.flight, p.launched, p.secondary, p.owner});
	}
	this->projectile_count.in_flight = this->projectiles.size();
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	this->projectile_snapshot = std::move(views);
	this->projectile_version += 1;
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

stance_t CombatState::get_stance(entity_id_t id) const {
	auto c = this->find(id);
	return c ? c->stance : stance_t::AGGRESSIVE;
}

order_t CombatState::get_order(entity_id_t id) const {
	auto c = this->find(id);
	return c ? c->order : order_t::NONE;
}

formation_t CombatState::get_formation(entity_id_t id) const {
	auto c = this->find(id);
	return c ? c->formation : formation_t::NONE;
}

std::optional<entity_id_t> CombatState::garrisoned_in(entity_id_t id) const {
	auto c = this->find(id);
	return c ? c->inside : std::nullopt;
}

std::optional<entity_id_t> CombatState::entering(entity_id_t id) const {
	auto c = this->find(id);
	return c ? c->entering : std::nullopt;
}

std::vector<entity_id_t> CombatState::occupants(entity_id_t building) const {
	auto c = this->find(building);
	return c ? c->occupants : std::vector<entity_id_t>{};
}

int CombatState::arrow_count_of(entity_id_t building) const {
	auto c = this->find(building);
	if (c == nullptr or not c->stats->shoots_arrows()) {
		return 0;
	}
	return arrow_count(this->running_arrows(*c, this->clock), c->stats->max_projectiles);
}

bool CombatState::is_fleeing(entity_id_t id) const {
	auto c = this->find(id);
	return c and c->fleeing;
}

CombatState::ProjectileStatsCount CombatState::projectile_counts() const {
	auto counts = this->projectile_count;
	counts.in_flight = this->projectiles.size();
	return counts;
}

bool CombatState::was_removed(entity_id_t id) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	return this->removed.contains(id);
}

bool CombatState::is_hidden(entity_id_t id) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	return this->hidden_snapshot->contains(id);
}

std::shared_ptr<const std::unordered_set<entity_id_t>> CombatState::hidden_entities(uint64_t &version) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	version = this->hidden_version;
	return this->hidden_snapshot;
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

std::shared_ptr<const CombatStats> CombatState::get_stats_snapshot(entity_id_t id) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	auto it = this->stats_snapshot.find(id);
	return it == this->stats_snapshot.end() ? nullptr : it->second;
}

std::optional<CombatInfo> CombatState::get_info(entity_id_t id) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	auto it = this->info_snapshot.find(id);
	return it == this->info_snapshot.end() ? std::nullopt : std::optional<CombatInfo>{it->second};
}

std::vector<ProjectileView> CombatState::get_projectiles(uint64_t &version) const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	version = this->projectile_version;
	return this->projectile_snapshot;
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
