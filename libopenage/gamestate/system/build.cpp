// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "build.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include <nyan/nyan.h>

#include "log/log.h"
#include "log/message.h"

#include "coord/phys.h"
#include "coord/tile.h"
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/constructable.h"
#include "gamestate/component/api/move.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/build.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/econ_math.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"
#include "gamestate/production.h"
#include "gamestate/production_math.h"
#include "gamestate/system/move.h"


namespace openage::gamestate::system {

namespace {

/// longest building step (seconds)
constexpr double MAX_STEP = 1.0;

/// approach tiles tried per walk (each try runs the pathfinder)
constexpr size_t MAX_APPROACH_TRIES = 4;

/// failed walks in a row before the job is given up
constexpr int MAX_FAILED_WALKS = 3;

/// minimum time between INFO logs of a unit (seconds)
constexpr double UNIT_LOG_INTERVAL = 10.0;

using BuilderComp = component::Builder;
using phase_t = component::Builder::phase_t;

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

std::shared_ptr<component::Constructable> constructable_of(const std::shared_ptr<GameEntity> &entity) {
	if (entity == nullptr or not entity->has_component(component::component_t::CONSTRUCTABLE)) {
		return nullptr;
	}
	return std::dynamic_pointer_cast<component::Constructable>(
		entity->get_component(component::component_t::CONSTRUCTABLE));
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

/**
 * Walk next to a foundation (same rule as the gather system).
 *
 * @return Walk duration, 0 if the unit already stands next to it, nothing if no path was found.
 */
std::optional<time::time_t> walk_next_to(const std::shared_ptr<GameEntity> &entity,
                                         const std::shared_ptr<GameState> &state,
                                         const coord::phys3 &target,
                                         double radius,
                                         const time::time_t &time) {
	if (not entity->has_component(component::component_t::MOVE)) {
		return std::nullopt;
	}
	auto move = std::dynamic_pointer_cast<component::Move>(
		entity->get_component(component::component_t::MOVE));
	auto path_type = move->get_ability().get<nyan::ObjectValue>("Move.path_type");
	auto grid = state->get_map()->get_grid_id(path_type->get_name());

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
		if (not map->is_passable(grid, tile)) {
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

time::time_t end_job(BuilderComp &builder) {
	builder.get_job() = BuilderComp::Job{};
	return time::time_t::from_int(0);
}

/**
 * Continue the job: walk to the foundation or build the next step.
 */
time::time_t continue_build(const std::shared_ptr<GameEntity> &entity,
                            const std::shared_ptr<GameState> &state,
                            BuilderComp &builder,
                            const time::time_t &time) {
	auto &job = builder.get_job();
	auto target = job.target ? find_entity(state, *job.target) : nullptr;
	auto constructable = constructable_of(target);
	if (constructable == nullptr or constructable->is_complete()) {
		return end_job(builder);
	}

	auto target_pos = position_of(target, time);
	auto pos = position_of(entity, time);
	double slack = job.phase == phase_t::TO_SITE ? 0.5 : 0.0;
	if (not econ::in_reach(pos.ne.to_double(), pos.se.to_double(),
	                       target_pos.ne.to_double(), target_pos.se.to_double(), constructable->radius, slack)) {
		auto walk = walk_next_to(entity, state, target_pos, constructable->radius, time);
		if (walk and *walk > 0) {
			job.phase = phase_t::TO_SITE;
			job.failed_walks = 0;
			return *walk;
		}
		job.failed_walks += 1;
		if (job.failed_walks >= MAX_FAILED_WALKS) {
			log::log(INFO << "Build: unit " << entity->get_id() << " cannot reach foundation "
			              << target->get_id() << ", idle");
			return end_job(builder);
		}
		job.phase = phase_t::TO_SITE;
		return time::time_t::from_double(0.1);
	}

	// build the next step
	auto pos_component = std::dynamic_pointer_cast<component::Position>(
		entity->get_component(component::component_t::POSITION));
	auto to_target = target_pos - pos;
	if (to_target.length() > 1e-3) {
		pos_component->set_position(time, pos);
		pos_component->set_angle(time, to_target.to_angle());
	}
	if (job.phase != phase_t::BUILDING) {
		if (not builder.animation.empty()) {
			entity->render_update(time, builder.animation);
		}
		if (time >= builder.last_log + UNIT_LOG_INTERVAL or time < builder.last_log) {
			builder.last_log = time;
			log::log(INFO << "Build: unit " << entity->get_id() << " builds " << prod::entity_name(target)
			              << " (entity " << target->get_id() << ") at tile " << tile_str(target_pos)
			              << ", progress " << static_cast<int>(std::lround(100.0 * constructable->get_progress()))
			              << " %" << ", health "
			              << prod::construction_health(constructable->max_health, constructable->get_progress()) << "/"
			              << constructable->max_health);
		}
	}
	const double remaining = (1.0 - constructable->get_progress()) * constructable->get_build_time();
	job.phase = phase_t::BUILDING;
	job.step = std::clamp(remaining, 0.05, MAX_STEP);
	job.failed_walks = 0;
	return time::time_t::from_double(job.step);
}

} // namespace


const time::time_t Build::build_command(const std::shared_ptr<gamestate::GameEntity> &entity,
                                        const std::shared_ptr<openage::gamestate::GameState> &state,
                                        const time::time_t &start_time) {
	auto command_queue = std::dynamic_pointer_cast<component::CommandQueue>(
		entity->get_component(component::component_t::COMMANDQUEUE));
	auto command = std::dynamic_pointer_cast<component::command::BuildCommand>(
		command_queue->pop_command(start_time));

	if (not command) [[unlikely]] {
		log::log(MSG(warn) << "Command is not a build command.");
		return time::time_t::from_int(0);
	}
	if (not entity->has_component(component::component_t::BUILDER)) [[unlikely]] {
		log::log(WARN << "Entity " << entity->get_id() << " cannot build.");
		return time::time_t::from_int(0);
	}
	auto builder = std::dynamic_pointer_cast<component::Builder>(
		entity->get_component(component::component_t::BUILDER));

	auto target = find_entity(state, command->get_target());
	if (not prod::can_build(entity, target, start_time)) {
		log::log(INFO << "Build: unit " << entity->get_id() << " cannot build entity " << command->get_target());
		return end_job(*builder);
	}

	auto &job = builder->get_job();
	job = BuilderComp::Job{};
	job.target = target->get_id();
	builder->last_log = time::TIME_MIN;
	log::log(INFO << "Build: unit " << entity->get_id() << " ordered to build " << prod::entity_name(target)
	              << " (entity " << target->get_id() << ") at tile " << tile_str(position_of(target, start_time)));

	return continue_build(entity, state, *builder, start_time);
}

const time::time_t Build::build_step(const std::shared_ptr<gamestate::GameEntity> &entity,
                                     const std::shared_ptr<openage::gamestate::GameState> &state,
                                     const time::time_t &start_time) {
	if (not entity->has_component(component::component_t::BUILDER)) [[unlikely]] {
		return time::time_t::from_int(0);
	}
	auto builder = std::dynamic_pointer_cast<component::Builder>(
		entity->get_component(component::component_t::BUILDER));
	auto &job = builder->get_job();

	switch (job.phase) {
	case phase_t::NONE:
		return time::time_t::from_int(0);

	case phase_t::BUILDING: {
		// the step is done: add its work
		auto target = job.target ? find_entity(state, *job.target) : nullptr;
		auto constructable = constructable_of(target);
		if (constructable != nullptr and not constructable->is_complete()) {
			bool done = constructable->add_work(job.step);
			prod::update_building_health(target, start_time);
			if (done) {
				prod::complete_building(target, start_time);
				log::log(INFO << "Build: unit " << entity->get_id() << " completes " << prod::entity_name(target)
				              << " (entity " << target->get_id() << ")");
			}
		}
		job.step = 0.0;
		return continue_build(entity, state, *builder, start_time);
	}

	case phase_t::TO_SITE:
		return continue_build(entity, state, *builder, start_time);

	default:
		return end_job(*builder);
	}
}

bool Build::job_active(const std::shared_ptr<gamestate::GameEntity> &entity) {
	if (not entity->has_component(component::component_t::BUILDER)) {
		return false;
	}
	auto builder = std::dynamic_pointer_cast<component::Builder>(
		entity->get_component(component::component_t::BUILDER));
	return builder->get_job().phase != phase_t::NONE;
}

} // namespace openage::gamestate::system
