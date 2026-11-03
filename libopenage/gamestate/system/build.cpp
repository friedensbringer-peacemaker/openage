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
#include "gamestate/building_math.h"
#include "gamestate/building_place.h"
#include "gamestate/buildings.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/constructable.h"
#include "gamestate/component/api/drop_site.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/api/move.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/build.h"
#include "gamestate/component/internal/commands/gather.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/econ.h"
#include "gamestate/econ_math.h"
#include "gamestate/food.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"
#include "gamestate/player.h"
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

/// repair rate of one villager in HP per second (spec 04 §4.5: work rate 12.5 of the repair task)
constexpr double REPAIR_RATE = 12.5;

/// a searching builder looks again after this time (seconds)
constexpr double SEARCH_STEP = 0.5;

player_id_t owner_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto ownership = std::dynamic_pointer_cast<component::Ownership>(
		entity->get_component(component::component_t::OWNERSHIP));
	return ownership->get_owners().get(time);
}

/// current health of a building (Live attribute of its Constructable), nothing without one
std::optional<int64_t> health_of(const std::shared_ptr<GameEntity> &building,
                                 const component::Constructable &constructable,
                                 const time::time_t &time) {
	if (constructable.health_attribute.empty() or not building->has_component(component::component_t::LIVE)) {
		return std::nullopt;
	}
	auto live = std::dynamic_pointer_cast<component::Live>(building->get_component(component::component_t::LIVE));
	return live->get_attribute(time, constructable.health_attribute);
}

/**
 * XR fork W5: units working on a target in the same way (spec 04 §4.3/4.5: n of f(n)),
 * at least 1 (the asking unit itself).
 */
int workers_on(const std::shared_ptr<GameState> &state, entity_id_t target, bool repair) {
	int n = 0;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (not entity->has_component(component::component_t::BUILDER)) {
			continue;
		}
		auto b = std::dynamic_pointer_cast<component::Builder>(entity->get_component(component::component_t::BUILDER));
		const auto &job = b->get_job();
		if (job.phase == phase_t::BUILDING and job.target == target and job.repair == repair) {
			n += 1;
		}
	}
	return std::max(1, n);
}

/// the building still needs a repair: alive (spec 04 §4.5), finished, below its maximum
bool needs_repair(const std::shared_ptr<GameState> &state,
                  const std::shared_ptr<GameEntity> &target,
                  const component::Constructable &constructable,
                  const time::time_t &time) {
	if (not constructable.is_complete() or constructable.max_health <= 0) {
		return false;
	}
	if (auto combat = state->get_combat(); combat != nullptr and combat->is_dead(target->get_id())) {
		return false;
	}
	auto health = health_of(target, constructable, time);
	return health and building::can_repair(static_cast<double>(*health)) and *health < constructable.max_health;
}

void add_build_command(const std::shared_ptr<GameEntity> &entity, entity_id_t target, const time::time_t &time) {
	auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
		entity->get_component(component::component_t::COMMANDQUEUE));
	queue->add_command(time, std::make_shared<component::command::BuildCommand>(target));
}

/**
 * XR fork W5: one repair step of a unit (spec 04 §4.5): HP after f(n) with the half factor,
 * costs build cost * hp / (2 * hp_max) paid by the owner of the building as whole units;
 * if the owner cannot pay, the repair stops with a message.
 *
 * @return false if the job ended.
 */
bool repair_step(const std::shared_ptr<GameEntity> &entity,
                 const std::shared_ptr<GameState> &state,
                 BuilderComp &builder,
                 const std::shared_ptr<GameEntity> &target,
                 component::Constructable &constructable,
                 const time::time_t &time) {
	auto &job = builder.get_job();
	if (not needs_repair(state, target, constructable, time)) {
		return false;
	}
	const int64_t health = *health_of(target, constructable, time);
	const int n = workers_on(state, target->get_id(), true);
	double gain = building::repair_hp(REPAIR_RATE, 1.0, job.step, n, false);
	gain = std::min(gain, static_cast<double>(constructable.max_health - health) - job.repair_hp);
	if (gain <= 0.0) {
		return true;
	}
	// costs: the build cost of the building for the owner (data of the Create ability)
	resource_amounts_t cost{};
	if (const auto *buildable = builder.find(prod::entity_name(target))) {
		cost = buildable->cost;
	}
	const auto owner = owner_of(target, time);
	auto due = building::repair_due(job.repair_owed, cost, gain, static_cast<double>(constructable.max_health));
	resource_amounts_t pay{};
	for (size_t i = 0; i < RESOURCE_COUNT and i < due.size(); ++i) {
		pay[i] = due[i];
	}
	if (state->has_player(owner) and not state->get_player(owner)->get_resources().spend(pay)) {
		auto missing = prod::missing_resource(state->get_player(owner)->get_resources().get(), pay);
		const std::string text = "Reparatur abgebrochen: "
		                         + (missing ? prod::missing_message(*missing) : std::string{"Nicht genug Rohstoffe"});
		if (auto buildings = state->get_buildings()) {
			buildings->post(owner_of(entity, time), text, true);
		}
		log::log(INFO << "Build: unit " << entity->get_id() << " stops repairing " << prod::entity_name(target)
		              << " (entity " << target->get_id() << "): player " << owner << " cannot pay");
		end_job(builder);
		return false;
	}
	job.repair_hp += gain;
	const double whole = std::floor(job.repair_hp + 1e-9);
	if (whole >= 1.0) {
		job.repair_hp -= whole;
		const int64_t value = std::min<int64_t>(constructable.max_health, health + static_cast<int64_t>(whole));
		auto live = std::dynamic_pointer_cast<component::Live>(target->get_component(component::component_t::LIVE));
		live->set_attribute(time, constructable.health_attribute, value);
		if (auto combat = state->get_combat()) {
			combat->health_changed(target->get_id(), value);
		}
		if (value >= constructable.max_health) {
			log::log(INFO << "Build: unit " << entity->get_id() << " repaired " << prod::entity_name(target)
			              << " (entity " << target->get_id() << ") to " << value << "/" << constructable.max_health);
		}
	}
	return true;
}

/**
 * XR fork W5: what a builder does after its building is finished (spec 04 §4.3/4.4, L33):
 * the next wall segment of its drag (nearest first), the matching resource next to a camp,
 * otherwise a search for a foundation nearby (phase SEARCH).
 *
 * @return true if the job changed (a command was queued or the search started).
 */
bool follow_after_build(const std::shared_ptr<GameEntity> &entity,
                        const std::shared_ptr<GameState> &state,
                        BuilderComp &builder,
                        const std::shared_ptr<GameEntity> &finished,
                        const time::time_t &time) {
	auto pos = position_of(entity, time);
	// 1. walls: the nearest open segment of the drag
	auto &queue = builder.wall_queue;
	std::erase(queue, finished->get_id());
	std::vector<std::pair<double, double>> open;
	std::vector<entity_id_t> open_ids;
	for (auto id : queue) {
		auto e = find_entity(state, id);
		auto c = constructable_of(e);
		if (c == nullptr or c->is_complete() or (state->get_combat() and state->get_combat()->is_dead(id))) {
			continue;
		}
		auto p = position_of(e, time);
		open.emplace_back(p.ne.to_double(), p.se.to_double());
		open_ids.push_back(id);
	}
	queue = open_ids;
	if (auto next = building::nearest_segment(pos.ne.to_double(), pos.se.to_double(), open)) {
		add_build_command(entity, open_ids[*next], time);
		log::log(INFO << "Build: unit " << entity->get_id() << " builds the next wall segment " << open_ids[*next]
		              << " (" << open_ids.size() << " left)");
		return true;
	}

	// 2. camps: gather the matching resource nearby (L33)
	std::array<bool, 4> accepts{};
	if (finished->has_component(component::component_t::DROP_SITE)) {
		auto drop = std::dynamic_pointer_cast<component::DropSite>(finished->get_component(component::component_t::DROP_SITE));
		for (size_t i = 0; i < accepts.size(); ++i) {
			accepts[i] = drop->accepts(static_cast<resource_t>(i));
		}
	}
	const bool field = finished->has_component(component::component_t::HARVESTABLE);
	const auto follow = building::follow_up(field, accepts);
	if (follow == building::follow_t::FARM) {
		return false;
	}
	if (follow == building::follow_t::GATHER) {
		auto fpos = position_of(finished, time);
		std::shared_ptr<GameEntity> best;
		double best_d = building::CAMP_RESOURCE_RADIUS * building::CAMP_RESOURCE_RADIUS;
		for (const auto &[id, e] : state->get_game_entities()) {
			if (not e->has_component(component::component_t::HARVESTABLE)) {
				continue;
			}
			auto h = std::dynamic_pointer_cast<component::Harvestable>(e->get_component(component::component_t::HARVESTABLE));
			const auto r = static_cast<size_t>(h->get_resource());
			if (h->is_depleted() or r >= accepts.size() or not accepts[r] or not econ::can_gather_from(entity, e)) {
				continue;
			}
			const double d = distance2(position_of(e, time), fpos);
			if (d < best_d) {
				best_d = d;
				best = e;
			}
		}
		if (best != nullptr) {
			auto commands = std::dynamic_pointer_cast<component::CommandQueue>(
				entity->get_component(component::component_t::COMMANDQUEUE));
			commands->add_command(time, std::make_shared<component::command::GatherCommand>(best->get_id()));
			log::log(INFO << "Build: unit " << entity->get_id() << " gathers at " << prod::entity_name(best) << " (entity "
			              << best->get_id() << ") next to the " << prod::entity_name(finished) << " it built");
			return true;
		}
	}
	// 3. other buildings: look for a foundation nearby for 3 s (spec 04 §4.3)
	auto &job = builder.get_job();
	job = BuilderComp::Job{};
	job.phase = phase_t::SEARCH;
	job.search_until = time.to_double() + building::FOLLOW_SEARCH_TIME;
	return true;
}

/// XR fork W5: phase SEARCH – an own foundation nearby gets a build order, else idle after 3 s
time::time_t search_step(const std::shared_ptr<GameEntity> &entity,
                         const std::shared_ptr<GameState> &state,
                         BuilderComp &builder,
                         const time::time_t &time) {
	auto pos = position_of(entity, time);
	const auto owner = owner_of(entity, time);
	std::shared_ptr<GameEntity> best;
	double best_d = building::FOLLOW_SEARCH_RADIUS * building::FOLLOW_SEARCH_RADIUS;
	for (const auto &[id, e] : state->get_game_entities()) {
		auto c = constructable_of(e);
		if (c == nullptr or c->is_complete() or not e->has_component(component::component_t::OWNERSHIP)
		    or owner_of(e, time) != owner or (state->get_combat() and state->get_combat()->is_dead(id))) {
			continue;
		}
		const double d = distance2(position_of(e, time), pos);
		if (d < best_d) {
			best_d = d;
			best = e;
		}
	}
	if (best != nullptr) {
		log::log(INFO << "Build: unit " << entity->get_id() << " found the foundation " << prod::entity_name(best)
		              << " (entity " << best->get_id() << ") nearby");
		add_build_command(entity, best->get_id(), time);
		return end_job(builder);
	}
	if (time.to_double() >= builder.get_job().search_until - 1e-9) {
		log::log(INFO << "Build: unit " << entity->get_id() << " found no foundation nearby, idle");
		return end_job(builder);
	}
	return time::time_t::from_double(SEARCH_STEP);
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
	if (constructable == nullptr
	    or (job.repair ? not needs_repair(state, target, *constructable, time) : constructable->is_complete())) {
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
		if (job.repair) {
			builder.last_log = time;
			log::log(INFO << "Build: unit " << entity->get_id() << " repairs " << prod::entity_name(target)
			              << " (entity " << target->get_id() << ") at tile " << tile_str(target_pos) << ", health "
			              << health_of(target, *constructable, time).value_or(0) << "/" << constructable->max_health);
		}
		else if (time >= builder.last_log + UNIT_LOG_INTERVAL or time < builder.last_log) {
			builder.last_log = time;
			log::log(INFO << "Build: unit " << entity->get_id() << " builds " << prod::entity_name(target)
			              << " (entity " << target->get_id() << ") at tile " << tile_str(target_pos)
			              << ", progress " << static_cast<int>(std::lround(100.0 * constructable->get_progress()))
			              << " %" << ", health "
			              << prod::construction_health(constructable->max_health, constructable->get_progress()) << "/"
			              << constructable->max_health);
		}
	}
	double remaining = (1.0 - constructable->get_progress()) * constructable->get_build_time();
	if (job.repair) {
		// XR fork W5: steps of at most a second until the building is whole
		auto health = health_of(target, *constructable, time).value_or(constructable->max_health);
		remaining = static_cast<double>(constructable->max_health - health) / REPAIR_RATE;
	}
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

	// XR fork W3: a build order ends the gather job (otherwise the unit still counts as a gatherer
	// and holds its farm after the building is done)
	if (entity->has_component(component::component_t::GATHER)) {
		auto gather = std::dynamic_pointer_cast<component::Gather>(entity->get_component(component::component_t::GATHER));
		auto &gjob = gather->get_job();
		if (gjob.target and state->get_food()) {
			state->get_food()->release(find_entity(state, *gjob.target), entity->get_id());
		}
		gjob = component::Gather::Job{};
	}

	auto &job = builder->get_job();
	job = BuilderComp::Job{};
	job.target = target->get_id();
	// XR fork W5: a finished building is repaired (prod::can_build allows damaged own ones)
	job.repair = constructable_of(target)->is_complete();
	// XR fork W5: a segment of the wall drag stays in the queue order, any other order ends the drag
	auto &walls = builder->wall_queue;
	if (std::find(walls.begin(), walls.end(), target->get_id()) == walls.end()) {
		walls.clear();
	}
	else {
		std::erase(walls, target->get_id());
	}
	builder->last_log = time::TIME_MIN;
	log::log(INFO << "Build: unit " << entity->get_id() << " ordered to " << (job.repair ? "repair " : "build ")
	              << prod::entity_name(target) << " (entity " << target->get_id() << ") at tile "
	              << tile_str(position_of(target, start_time)));

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
		if (job.repair) {
			// XR fork W5: repair (spec 04 §4.5)
			if (constructable == nullptr or not repair_step(entity, state, *builder, target, *constructable, start_time)) {
				return end_job(*builder);
			}
			job.step = 0.0;
			return continue_build(entity, state, *builder, start_time);
		}
		bool farmer = false;
		if (constructable != nullptr and not constructable->is_complete()) {
			const double before = constructable->get_progress();
			// XR fork W5: p = W * a1 * dt * k * f(n) (spec 04 §4.3), n = units building it now
			const auto owner = owner_of(target, start_time);
			const double k = state->get_buildings() ? state->get_buildings()->build_speed(owner) : 1.0;
			const int n = workers_on(state, target->get_id(), false);
			const double work = building::build_progress(1.0, 1.0, job.step, k, n, prod::entity_name(target) == "Wonder");
			bool done = constructable->add_work(work);
			prod::update_building_health(state, target, before, start_time);
			auto food_state = state->get_food();
			if (food_state) {
				// XR fork W3: the ground of a farm shows the construction stages of the data
				food_state->field_progress(state, target, start_time);
			}
			if (done) {
				prod::complete_building(target, start_time);
				log::log(INFO << "Build: unit " << entity->get_id() << " completes " << prod::entity_name(target)
				              << " (entity " << target->get_id() << ") with " << n << " builders");
				// XR fork W5: a gate opens when it is built (unless locked)
				if (auto buildings = state->get_buildings(); buildings and buildings->gate(target->get_id())) {
					buildings->gate_finished(state, target->get_id(), start_time);
				}
				if (food_state) {
					// XR fork W3: a finished farm gets its food; its builder farms it (spec 13 L33)
					food_state->field_complete(state, target, start_time);
					if (econ::can_gather_from(entity, target) and food_state->claim(target, entity->get_id())) {
						auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
							entity->get_component(component::component_t::COMMANDQUEUE));
						queue->add_command(start_time,
						                   std::make_shared<component::command::GatherCommand>(target->get_id()));
						log::log(INFO << "Build: unit " << entity->get_id() << " works the field "
						              << target->get_id() << " it built");
						farmer = true;
					}
				}
			}
		}
		// XR fork W5: every builder of a finished building follows up (next wall segment, camp, search; L33)
		if (constructable != nullptr and constructable->is_complete() and not farmer
		    and follow_after_build(entity, state, *builder, target, start_time)) {
			if (job.phase == phase_t::SEARCH) {
				return time::time_t::from_double(SEARCH_STEP);
			}
			return end_job(*builder);
		}
		job.step = 0.0;
		return continue_build(entity, state, *builder, start_time);
	}

	case phase_t::TO_SITE:
		return continue_build(entity, state, *builder, start_time);

	case phase_t::SEARCH:
		return search_step(entity, state, *builder, start_time);

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
