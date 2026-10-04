// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "gather.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

#include <nyan/nyan.h>

#include "log/log.h"
#include "log/message.h"

#include "coord/phys.h"
#include "coord/tile.h"
#include "gamestate/api/ability.h"
#include "gamestate/api/animation.h"
#include "gamestate/api/property.h"
#include "gamestate/api/types.h"
#include "gamestate/component/api/drop_site.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/api/idle.h"
#include "gamestate/component/api/move.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/gather.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/econ_math.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"
#include "gamestate/player.h"
#include "gamestate/system/move.h"
#include "util/fixed_point.h"


namespace openage::gamestate::system {

namespace {

/// search radius (tiles) for the next resource of the same type
constexpr double RESUME_SEARCH_RANGE = 12.0;

/// approach tiles tried per walk (each try runs the pathfinder)
constexpr size_t MAX_APPROACH_TRIES = 4;

/// failed walks in a row before the job is given up
constexpr int MAX_FAILED_WALKS = 3;

/// minimum time between INFO logs of a unit (seconds)
constexpr double UNIT_LOG_INTERVAL = 5.0;

/// interval of the player resource log (seconds)
constexpr double RESOURCE_LOG_INTERVAL = 10.0;

using GatherComp = component::Gather;
using phase_t = component::Gather::phase_t;

std::shared_ptr<GameEntity> find_entity(const std::shared_ptr<GameState> &state, entity_id_t id) {
	const auto &entities = state->get_game_entities();
	auto it = entities.find(id);
	if (it == entities.end()) {
		return nullptr;
	}
	return it->second;
}

coord::phys3 position_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto pos = std::dynamic_pointer_cast<component::Position>(
		entity->get_component(component::component_t::POSITION));
	return pos->get_positions().get(time);
}

player_id_t owner_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto owner = std::dynamic_pointer_cast<component::Ownership>(
		entity->get_component(component::component_t::OWNERSHIP));
	return owner->get_owners().get(time);
}

std::shared_ptr<component::Harvestable> harvestable_of(const std::shared_ptr<GameEntity> &entity) {
	if (entity == nullptr or not entity->has_component(component::component_t::HARVESTABLE)) {
		return nullptr;
	}
	return std::dynamic_pointer_cast<component::Harvestable>(
		entity->get_component(component::component_t::HARVESTABLE));
}

std::shared_ptr<component::DropSite> drop_site_of(const std::shared_ptr<GameEntity> &entity) {
	if (entity == nullptr or not entity->has_component(component::component_t::DROP_SITE)) {
		return nullptr;
	}
	return std::dynamic_pointer_cast<component::DropSite>(
		entity->get_component(component::component_t::DROP_SITE));
}

double distance2(const coord::phys3 &a, const coord::phys3 &b) {
	double dx = a.ne.to_double() - b.ne.to_double();
	double dy = a.se.to_double() - b.se.to_double();
	return dx * dx + dy * dy;
}

std::string tile_str(const coord::phys3 &pos) {
	return "(" + std::to_string(static_cast<int>(std::floor(pos.ne.to_double()))) + ", "
	       + std::to_string(static_cast<int>(std::floor(pos.se.to_double()))) + ")";
}

/// first animation of an Animated ability (empty if none)
std::string animation_of(const nyan::Object &ability) {
	if (api::APIAbility::check_property(ability, api::ability_property_t::ANIMATED)) {
		auto property = api::APIAbility::get_property(ability, api::ability_property_t::ANIMATED);
		auto animations = api::APIAbilityProperty::get_animations(property);
		auto paths = api::APIAnimation::get_animation_paths(animations);
		if (not paths.empty()) {
			return paths[0];
		}
	}
	return {};
}

/// path grid of a moving unit
std::optional<path::grid_id_t> grid_of(const std::shared_ptr<GameEntity> &entity,
                                       const std::shared_ptr<GameState> &state) {
	if (not entity->has_component(component::component_t::MOVE)) {
		return std::nullopt;
	}
	auto move = std::dynamic_pointer_cast<component::Move>(
		entity->get_component(component::component_t::MOVE));
	auto path_type = move->get_ability().get<nyan::ObjectValue>("Move.path_type");
	return state->get_map()->get_grid_id(path_type->get_name());
}

/// true if a free tile next to the object exists on the unit's grid
bool has_free_side(const std::shared_ptr<GameState> &state,
                   path::grid_id_t grid,
                   const coord::phys3 &pos,
                   double radius) {
	auto ne = pos.ne.to_double();
	auto se = pos.se.to_double();
	for (const auto &t : econ::approach_tiles(ne, se, radius, ne, se)) {
		if (state->get_map()->is_passable(grid, coord::tile{t.ne, t.se})) {
			return true;
		}
	}
	return false;
}

/**
 * Walk next to an object (resource or drop site).
 *
 * @return Walk duration, 0 if the unit already stands next to it, nothing if no path was found.
 */
std::optional<time::time_t> walk_next_to(const std::shared_ptr<GameEntity> &entity,
                                         const std::shared_ptr<GameState> &state,
                                         const coord::phys3 &target,
                                         double radius,
                                         const time::time_t &time) {
	auto grid = grid_of(entity, state);
	if (not grid) {
		return std::nullopt;
	}
	auto pos = position_of(entity, time);
	auto ne = pos.ne.to_double();
	auto se = pos.se.to_double();
	if (econ::in_reach(ne, se, target.ne.to_double(), target.se.to_double(), radius)) {
		return time::time_t::from_int(0);
	}

	auto map = state->get_map();
	size_t tries = 0;
	for (const auto &t : econ::approach_tiles(target.ne.to_double(), target.se.to_double(), radius, ne, se)) {
		coord::tile tile{t.ne, t.se};
		if (not map->is_passable(*grid, tile)) {
			continue;
		}
		if (tries >= MAX_APPROACH_TRIES) {
			break;
		}
		tries += 1;

		auto dest = map->on_terrain(tile.to_phys3_center());
		auto duration = Move::move_default(entity, state, dest, time);
		auto end = position_of(entity, time + duration);
		if (distance2(end, dest) < 0.05) {
			return duration;
		}
	}
	return std::nullopt;
}

/// nearest resource of a type that has a free side (nothing if none in range)
std::shared_ptr<GameEntity> find_resource(const std::shared_ptr<GameState> &state,
                                          path::grid_id_t grid,
                                          resource_t type,
                                          const coord::phys3 &from,
                                          double range,
                                          const time::time_t &time,
                                          const std::shared_ptr<GameEntity> &exclude = nullptr) {
	std::shared_ptr<GameEntity> best;
	double best_d2 = range * range;
	for (const auto &[id, candidate] : state->get_game_entities()) {
		auto harvestable = harvestable_of(candidate);
		if (candidate == exclude or harvestable == nullptr or harvestable->is_depleted()
		    or harvestable->get_resource() != type) {
			continue;
		}
		auto pos = position_of(candidate, time);
		auto d2 = distance2(pos, from);
		if (d2 >= best_d2) {
			continue;
		}
		if (not has_free_side(state, grid, pos, harvestable->radius)) {
			continue;
		}
		best = candidate;
		best_d2 = d2;
	}
	return best;
}

/// nearest drop site of the player that accepts the resource
std::shared_ptr<GameEntity> find_drop_site(const std::shared_ptr<GameState> &state,
                                           player_id_t owner,
                                           resource_t type,
                                           const coord::phys3 &from,
                                           const time::time_t &time) {
	std::shared_ptr<GameEntity> best;
	double best_d2 = std::numeric_limits<double>::max();
	for (const auto &[id, candidate] : state->get_game_entities()) {
		auto drop_site = drop_site_of(candidate);
		if (drop_site == nullptr or not drop_site->accepts(type) or owner_of(candidate, time) != owner) {
			continue;
		}
		auto d2 = distance2(position_of(candidate, time), from);
		if (d2 < best_d2) {
			best = candidate;
			best_d2 = d2;
		}
	}
	return best;
}

/// remove an empty resource from the world: free its tiles, move it out of sight
void deplete(const std::shared_ptr<GameEntity> &target,
             const std::shared_ptr<GameState> &state,
             const time::time_t &time) {
	auto harvestable = harvestable_of(target);
	harvestable->set_depleted();

	auto pos = position_of(target, time);
	for (const auto &t : econ::footprint(pos.ne.to_double(), pos.se.to_double(), harvestable->radius)) {
		state->get_map()->unblock_tile(coord::tile{t.ne, t.se}, time);
	}

	// TODO: remove the entity and its render entity (the world renderer cannot remove objects yet)
	auto pos_component = std::dynamic_pointer_cast<component::Position>(
		target->get_component(component::component_t::POSITION));
	pos_component->set_position(time, pos);
	pos_component->set_position(time + 0.001, coord::phys3{-1000, -1000, 0});
	std::string animation;
	if (target->has_component(component::component_t::IDLE)) {
		auto idle = std::dynamic_pointer_cast<component::Idle>(
			target->get_component(component::component_t::IDLE));
		animation = animation_of(idle->get_ability());
	}
	if (not animation.empty()) {
		target->render_update(time, animation);
	}
	log::log(INFO << "Gather: resource " << target->get_id() << " at tile " << tile_str(pos)
	              << " is empty, removed");
}

/// INFO log of the player resources, at most every RESOURCE_LOG_INTERVAL per player
void log_resources(const std::shared_ptr<GameState> &state, player_id_t owner, const time::time_t &time, bool force) {
	static std::unordered_map<player_id_t, time::time_t> last_log;
	auto it = last_log.find(owner);
	if (not force and it != last_log.end() and time >= it->second
	    and time < it->second + RESOURCE_LOG_INTERVAL) {
		return;
	}
	last_log[owner] = time;
	auto amounts = state->get_player(owner)->get_resources().get();
	log::log(INFO << "Player " << owner << " resources at t=" << time.to_double() << " s: food "
	              << amounts[0] << ", wood " << amounts[1] << ", gold " << amounts[2] << ", stone " << amounts[3]);
}

bool unit_log_due(GatherComp &gather, const time::time_t &time) {
	if (time >= gather.last_log and time < gather.last_log + UNIT_LOG_INTERVAL) {
		return false;
	}
	gather.last_log = time;
	return true;
}

time::time_t end_job(GatherComp &gather) {
	gather.get_job() = GatherComp::Job{};
	return time::time_t::from_int(0);
}

time::time_t go_drop_off(const std::shared_ptr<GameEntity> &entity,
                         const std::shared_ptr<GameState> &state,
                         GatherComp &gather,
                         const time::time_t &time);

/**
 * Continue at the resource: gather the next chunk, walk to it, look for the next
 * resource of the same type, or carry the load home.
 */
time::time_t continue_at_resource(const std::shared_ptr<GameEntity> &entity,
                                  const std::shared_ptr<GameState> &state,
                                  GatherComp &gather,
                                  const time::time_t &time) {
	auto &job = gather.get_job();
	const auto &skill = gather.get_skill(job.resource);

	if (gather.get_carried() >= skill.capacity - 1e-6) {
		return go_drop_off(entity, state, gather, time);
	}

	auto target = job.target ? find_entity(state, *job.target) : nullptr;
	auto harvestable = harvestable_of(target);
	if (harvestable == nullptr or harvestable->is_depleted()) {
		// next resource of the same type near the old one
		auto grid = grid_of(entity, state);
		std::shared_ptr<GameEntity> next;
		if (grid) {
			next = find_resource(state, *grid, job.resource, job.target_pos, RESUME_SEARCH_RANGE, time);
		}
		if (next == nullptr) {
			if (gather.get_carried() > 0.0) {
				return go_drop_off(entity, state, gather, time);
			}
			log::log(INFO << "Gather: unit " << entity->get_id() << " finds no more "
			              << to_string(job.resource) << ", idle");
			return end_job(gather);
		}
		target = next;
		harvestable = harvestable_of(next);
		job.target = next->get_id();
		job.target_pos = position_of(next, time);
	}

	auto target_pos = position_of(target, time);
	auto pos = position_of(entity, time);
	// after a walk, the unit stands on the chosen tile next to the target
	double radius = harvestable->radius;
	double slack = job.phase == phase_t::TO_RESOURCE ? 0.5 : 0.0;
	if (not econ::in_reach(pos.ne.to_double(), pos.se.to_double(),
	                       target_pos.ne.to_double(), target_pos.se.to_double(), radius, slack)) {
		auto walk = walk_next_to(entity, state, target_pos, radius, time);
		if (walk and *walk > 0) {
			job.phase = phase_t::TO_RESOURCE;
			job.failed_walks = 0;
			return *walk;
		}
		job.failed_walks += 1;
		if (job.failed_walks >= MAX_FAILED_WALKS) {
			log::log(INFO << "Gather: unit " << entity->get_id() << " cannot reach "
			              << to_string(job.resource) << ", idle");
			return end_job(gather);
		}
		// unreachable: try another resource of the same type
		auto grid = grid_of(entity, state);
		std::shared_ptr<GameEntity> next;
		if (grid) {
			next = find_resource(state, *grid, job.resource, pos, RESUME_SEARCH_RANGE, time, target);
		}
		if (next == nullptr) {
			log::log(INFO << "Gather: unit " << entity->get_id() << " finds no path to "
			              << to_string(job.resource) << ", idle");
			return end_job(gather);
		}
		job.target = next->get_id();
		job.target_pos = position_of(next, time);
		// re-evaluated in the next step (after a short pause)
		job.phase = phase_t::TO_RESOURCE;
		return time::time_t::from_double(0.1);
	}

	// gather the next chunk
	auto [amount, seconds] = econ::gather_chunk(gather.get_carried(), skill.capacity, skill.rate);
	if (amount <= 0.0) {
		return go_drop_off(entity, state, gather, time);
	}
	auto pos_component = std::dynamic_pointer_cast<component::Position>(
		entity->get_component(component::component_t::POSITION));
	auto to_target = target_pos - pos;
	if (to_target.length() > 1e-3) {
		pos_component->set_position(time, pos);
		pos_component->set_angle(time, to_target.to_angle());
	}
	if (job.phase != phase_t::GATHERING) {
		if (not skill.animation.empty()) {
			entity->render_update(time, skill.animation);
		}
		if (unit_log_due(gather, time)) {
			log::log(INFO << "Gather: unit " << entity->get_id() << " gathers " << to_string(job.resource)
			              << " at entity " << target->get_id() << " tile " << tile_str(target_pos)
			              << " (left " << harvestable->get_amount() << ", rate " << skill.rate
			              << "/s, carrying " << gather.get_carried() << "/" << skill.capacity << ")");
		}
	}
	if (gather.get_carried_type() != job.resource) {
		gather.set_carried(job.resource, 0.0);
	}
	job.phase = phase_t::GATHERING;
	job.chunk = amount;
	job.failed_walks = 0;
	return time::time_t::from_double(seconds);
}

time::time_t go_drop_off(const std::shared_ptr<GameEntity> &entity,
                         const std::shared_ptr<GameState> &state,
                         GatherComp &gather,
                         const time::time_t &time) {
	auto &job = gather.get_job();
	auto pos = position_of(entity, time);
	auto owner = owner_of(entity, time);
	auto site = find_drop_site(state, owner, gather.get_carried_type(), pos, time);
	if (site == nullptr) {
		log::log(INFO << "Gather: unit " << entity->get_id() << " has no drop site for "
		              << to_string(gather.get_carried_type()) << ", idle");
		return end_job(gather);
	}
	auto site_pos = position_of(site, time);
	auto walk = walk_next_to(entity, state, site_pos, drop_site_of(site)->radius, time);
	if (not walk) {
		log::log(INFO << "Gather: unit " << entity->get_id() << " finds no path to drop site "
		              << site->get_id() << ", idle");
		return end_job(gather);
	}
	job.phase = phase_t::TO_DROP_SITE;
	job.drop_site = site->get_id();
	// at least a short step, the event loop must advance
	return std::max(*walk, time::time_t::from_double(0.1));
}

} // namespace


const time::time_t Gather::gather_command(const std::shared_ptr<gamestate::GameEntity> &entity,
                                          const std::shared_ptr<openage::gamestate::GameState> &state,
                                          const time::time_t &start_time) {
	auto command_queue = std::dynamic_pointer_cast<component::CommandQueue>(
		entity->get_component(component::component_t::COMMANDQUEUE));
	auto command = std::dynamic_pointer_cast<component::command::GatherCommand>(
		command_queue->pop_command(start_time));

	if (not command) [[unlikely]] {
		log::log(MSG(warn) << "Command is not a gather command.");
		return time::time_t::from_int(0);
	}
	if (not entity->has_component(component::component_t::GATHER)) [[unlikely]] {
		log::log(WARN << "Entity " << entity->get_id() << " cannot gather.");
		return time::time_t::from_int(0);
	}
	auto gather = std::dynamic_pointer_cast<component::Gather>(
		entity->get_component(component::component_t::GATHER));

	auto target = find_entity(state, command->get_target());
	auto harvestable = harvestable_of(target);
	if (harvestable == nullptr or harvestable->is_depleted()
	    or not gather->can_gather(harvestable->get_resource())) {
		log::log(INFO << "Gather: unit " << entity->get_id() << " cannot gather from entity "
		              << command->get_target());
		end_job(*gather);
		return time::time_t::from_int(0);
	}

	auto &job = gather->get_job();
	job = GatherComp::Job{};
	job.phase = phase_t::NONE;
	job.target = target->get_id();
	job.resource = harvestable->get_resource();
	job.target_pos = position_of(target, start_time);
	if (gather->get_carried() > 0.0 and gather->get_carried_type() != job.resource) {
		// AoE: switching the resource drops the load
		gather->set_carried(job.resource, 0.0);
	}
	gather->last_log = time::TIME_MIN;
	log::log(INFO << "Gather: unit " << entity->get_id() << " ordered to gather " << to_string(job.resource)
	              << " at entity " << target->get_id() << " tile " << tile_str(job.target_pos));

	return continue_at_resource(entity, state, *gather, start_time);
}

const time::time_t Gather::gather_step(const std::shared_ptr<gamestate::GameEntity> &entity,
                                       const std::shared_ptr<openage::gamestate::GameState> &state,
                                       const time::time_t &start_time) {
	if (not entity->has_component(component::component_t::GATHER)) [[unlikely]] {
		return time::time_t::from_int(0);
	}
	auto gather = std::dynamic_pointer_cast<component::Gather>(
		entity->get_component(component::component_t::GATHER));
	auto &job = gather->get_job();
	if (job.phase != phase_t::NONE) {
		// player resources every RESOURCE_LOG_INTERVAL while the economy runs
		log_resources(state, owner_of(entity, start_time), start_time, false);
	}

	switch (job.phase) {
	case phase_t::NONE:
		return time::time_t::from_int(0);

	case phase_t::GATHERING: {
		// the chunk is done: take it from the resource
		auto target = job.target ? find_entity(state, *job.target) : nullptr;
		auto harvestable = harvestable_of(target);
		if (harvestable != nullptr and not harvestable->is_depleted()) {
			auto taken = harvestable->take(job.chunk);
			gather->set_carried(job.resource, gather->get_carried() + taken);
			if (harvestable->get_amount() <= 0.0) {
				deplete(target, state, start_time);
			}
		}
		job.chunk = 0.0;
		return continue_at_resource(entity, state, *gather, start_time);
	}

	case phase_t::TO_RESOURCE:
		return continue_at_resource(entity, state, *gather, start_time);

	case phase_t::TO_DROP_SITE: {
		auto site = job.drop_site ? find_entity(state, *job.drop_site) : nullptr;
		auto drop_site = drop_site_of(site);
		auto pos = position_of(entity, start_time);
		if (drop_site == nullptr) {
			return go_drop_off(entity, state, *gather, start_time);
		}
		auto site_pos = position_of(site, start_time);
		if (not econ::in_reach(pos.ne.to_double(), pos.se.to_double(),
		                       site_pos.ne.to_double(), site_pos.se.to_double(), drop_site->radius, 0.5)) {
			job.failed_walks += 1;
			if (job.failed_walks >= MAX_FAILED_WALKS) {
				log::log(INFO << "Gather: unit " << entity->get_id() << " cannot reach drop site, idle");
				return end_job(*gather);
			}
			return go_drop_off(entity, state, *gather, start_time);
		}

		auto owner = owner_of(entity, start_time);
		auto type = gather->get_carried_type();
		auto amount = gather->get_carried();
		auto total = state->get_player(owner)->get_resources().add(type, amount);
		gather->set_carried(type, 0.0);
		job.drop_site.reset();
		job.failed_walks = 0;
		if (unit_log_due(*gather, start_time)) {
			log::log(INFO << "Gather: unit " << entity->get_id() << " drops off " << amount << " "
			              << to_string(type) << " at entity " << site->get_id() << " -> player " << owner
			              << " " << to_string(type) << " " << total);
		}
		log_resources(state, owner, start_time, false);

		// back to the resource (the walk starts in continue_at_resource)
		job.phase = phase_t::NONE;
		auto duration = continue_at_resource(entity, state, *gather, start_time);
		return std::max(duration, time::time_t::from_double(0.1));
	}

	default:
		return end_job(*gather);
	}
}

bool Gather::job_active(const std::shared_ptr<gamestate::GameEntity> &entity) {
	if (not entity->has_component(component::component_t::GATHER)) {
		return false;
	}
	auto gather = std::dynamic_pointer_cast<component::Gather>(
		entity->get_component(component::component_t::GATHER));
	return gather->get_job().phase != phase_t::NONE;
}

} // namespace openage::gamestate::system
