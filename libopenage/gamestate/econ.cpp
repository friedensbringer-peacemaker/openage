// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "econ.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>

#include "log/log.h"
#include "log/message.h"

#include "coord/phys.h"
#include "coord/scene.h"
#include "gamestate/activity/activity.h"
#include "gamestate/activity/condition/command_in_queue.h"
#include "gamestate/activity/condition/next_command.h"
#include "gamestate/activity/end_node.h"
#include "gamestate/activity/event/command_in_queue.h"
#include "gamestate/activity/event/wait.h"
#include "gamestate/activity/start_node.h"
#include "gamestate/activity/task_node.h"
#include "gamestate/activity/task_system_node.h"
#include "gamestate/activity/xor_event_gate.h"
#include "gamestate/activity/xor_gate.h"
#include "gamestate/api/ability.h"
#include "gamestate/api/animation.h"
#include "gamestate/api/property.h"
#include "gamestate/api/types.h"
#include "gamestate/component/api/drop_site.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/types.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/econ_math.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/resources.h"
#include "gamestate/system/build.h"
#include "gamestate/system/gather.h"
#include "gamestate/system/types.h"
#include "renderer/camera/definitions.h"


namespace openage::gamestate::econ {

namespace {

/// gather rates if the data has none (AoE II: wood 0.39/s, food 0.31/s, gold and stone 0.38/s)
constexpr std::array<double, RESOURCE_COUNT> DEFAULT_RATES{0.31, 0.39, 0.38, 0.38};

/// default carry capacity
constexpr double DEFAULT_CAPACITY = 10.0;

std::optional<resource_t> resource_of(const nyan::Object &obj, const nyan::memberid_t &member) {
	return resource_from_name(obj.get_object(member).get_name());
}

/**
 * The converter writes sprites in game_entity/shared/ as "../shared/..." relative to
 * game_entity/generic/<unit>/, which points to game_entity/generic/shared/ (missing,
 * e.g. the mining animation of villagers). Point them to game_entity/shared/.
 */
std::string fix_shared_path(const std::string &path) {
	const std::string generic = "/game_entity/generic/";
	auto start = path.find(generic);
	if (start == std::string::npos) {
		return path;
	}
	auto unit_end = path.find('/', start + generic.size());
	const std::string up = "/../shared/";
	if (unit_end == std::string::npos or path.compare(unit_end, up.size(), up) != 0) {
		return path;
	}
	return path.substr(0, start) + "/game_entity/shared/" + path.substr(unit_end + up.size());
}

/// first animation of an Animated ability (empty if none)
std::string animation_of(const nyan::Object &ability) {
	if (api::APIAbility::check_property(ability, api::ability_property_t::ANIMATED)) {
		auto property = api::APIAbility::get_property(ability, api::ability_property_t::ANIMATED);
		auto animations = api::APIAbilityProperty::get_animations(property);
		auto paths = api::APIAnimation::get_animation_paths(animations);
		if (not paths.empty()) {
			return fix_shared_path(paths[0]);
		}
	}
	return {};
}

/// true for the gather ability that is preferred for food (berries, the start resource)
bool preferred_food_ability(const nyan::fqon_t &name) {
	const std::string suffix = ".CollectBerries";
	return name.size() >= suffix.size() and name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

void init_components(const std::shared_ptr<openage::event::EventLoop> &loop,
                     const std::shared_ptr<nyan::View> &owner_db_view,
                     const std::shared_ptr<GameEntity> &entity,
                     const nyan::fqon_t &nyan_entity) {
	auto nyan_obj = owner_db_view->get_object(nyan_entity);
	nyan::set_t abilities = nyan_obj.get_set("GameEntity.abilities");

	std::shared_ptr<component::Gather> gather;
	std::array<bool, RESOURCE_COUNT> preferred{};
	std::optional<nyan::Object> harvestable_ability;
	std::optional<nyan::Object> drop_site_ability;
	double radius = 0.5;
	double height = 1.0;

	for (const auto &ability_val : abilities) {
		auto ability_fqon = std::dynamic_pointer_cast<nyan::ObjectValue>(ability_val.get_ptr())->get_name();
		auto ability_obj = owner_db_view->get_object(ability_fqon);
		auto ability_parent = ability_obj.get_parents()[0];

		try {
			if (ability_parent == "engine.ability.type.Gather") {
				auto rate_obj = ability_obj.get_object("Gather.gather_rate");
				auto type = resource_of(rate_obj, "ResourceRate.type");
				if (not type) {
					continue;
				}
				auto idx = static_cast<size_t>(*type);
				bool prefer = *type == resource_t::FOOD and preferred_food_ability(ability_fqon);
				if (gather and gather->can_gather(*type) and (preferred[idx] or not prefer)) {
					// one ability per resource: the first one (berries for food)
					continue;
				}

				component::Gather::Skill skill;
				skill.available = true;
				skill.rate = rate_obj.get_float("ResourceRate.rate");
				if (not(skill.rate > 0.0)) {
					skill.rate = DEFAULT_RATES[idx];
				}
				skill.capacity = DEFAULT_CAPACITY;
				auto container = ability_obj.get_object("Gather.container");
				auto capacity = static_cast<double>(container.get_int("ResourceContainer.max_amount"));
				if (capacity > 0.0) {
					skill.capacity = capacity;
				}
				skill.animation = animation_of(ability_obj);

				if (not gather) {
					gather = std::make_shared<component::Gather>(loop, ability_obj);
				}
				gather->set_skill(*type, skill);
				preferred[idx] = prefer;
			}
			else if (ability_parent == "engine.ability.type.Harvestable") {
				harvestable_ability = ability_obj;
			}
			else if (ability_parent == "engine.ability.type.DropSite") {
				drop_site_ability = ability_obj;
			}
			else if (ability_parent == "engine.ability.type.Collision") {
				auto hitbox = ability_obj.get_object("Collision.hitbox");
				radius = hitbox.get_float("Hitbox.radius_x");
				height = hitbox.get_float("Hitbox.radius_z");
			}
		}
		catch (std::exception &err) {
			log::log(WARN << "Economy: ability " << ability_fqon << " of " << nyan_entity
			              << " not usable: " << err.what());
		}
	}

	if (gather) {
		entity->add_component(gather);
	}

	if (harvestable_ability) {
		try {
			auto spot = harvestable_ability->get_object("Harvestable.resources");
			auto type = resource_of(spot, "ResourceSpot.resource");
			if (type) {
				auto amount = static_cast<double>(spot.get_int("ResourceSpot.starting_amount"));
				auto harvestable = std::make_shared<component::Harvestable>(loop, *harvestable_ability, *type, amount);
				harvestable->radius = radius;
				harvestable->height = height;
				entity->add_component(harvestable);
			}
		}
		catch (std::exception &err) {
			log::log(WARN << "Economy: Harvestable of " << nyan_entity << " not usable: " << err.what());
		}
	}

	if (drop_site_ability) {
		std::array<bool, RESOURCE_COUNT> accepts{};
		try {
			for (const auto &container_val : drop_site_ability->get_set("DropSite.accepts_from")) {
				auto container_fqon = std::dynamic_pointer_cast<nyan::ObjectValue>(container_val.get_ptr())->get_name();
				auto container = owner_db_view->get_object(container_fqon);
				auto type = resource_of(container, "ResourceContainer.resource");
				if (type) {
					accepts[static_cast<size_t>(*type)] = true;
				}
			}
		}
		catch (std::exception &err) {
			log::log(WARN << "Economy: DropSite of " << nyan_entity << " not usable: " << err.what());
		}
		auto drop_site = std::make_shared<component::DropSite>(loop, *drop_site_ability, accepts);
		drop_site->radius = radius;
		entity->add_component(drop_site);
	}
}

std::shared_ptr<activity::Activity> gather_activity() {
	static std::mutex mutex;
	static std::shared_ptr<activity::Activity> cached;
	std::lock_guard<std::mutex> lock{mutex};
	if (cached) {
		return cached;
	}

	auto start = std::make_shared<activity::StartNode>(0);
	auto idle = std::make_shared<activity::TaskSystemNode>(1, "Idle");
	auto check_queue = std::make_shared<activity::XorGate>(2);
	auto wait_for_command = std::make_shared<activity::XorEventGate>(3);
	auto branch = std::make_shared<activity::XorGate>(4);
	auto move = std::make_shared<activity::TaskSystemNode>(5, "Move");
	auto wait_move = std::make_shared<activity::XorEventGate>(6);
	auto gather_command = std::make_shared<activity::TaskSystemNode>(7, "GatherCommand");
	auto gather_wait = std::make_shared<activity::XorEventGate>(8);
	auto gather_active = std::make_shared<activity::XorGate>(9);
	auto drop_command = std::make_shared<activity::TaskCustom>(10, "DropCommand");
	auto gather_step = std::make_shared<activity::TaskSystemNode>(11, "GatherStep");

	start->add_output(idle);

	idle->add_output(check_queue);
	idle->set_system_id(system::system_id_t::IDLE);

	check_queue->add_output(branch, activity::command_in_queue);
	check_queue->set_default(wait_for_command);

	wait_for_command->add_output(branch, activity::primer_command_in_queue);

	branch->add_output(move, activity::next_command_move);
	branch->add_output(gather_command, [](const time::time_t &time, const std::shared_ptr<GameEntity> &entity) {
		auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
			entity->get_component(component::component_t::COMMANDQUEUE));
		if (queue->get_queue().empty(time)) {
			return false;
		}
		return queue->get_queue().front(time)->get_type() == component::command::command_t::GATHER;
	});
	// commands this graph does not handle are dropped, otherwise Idle -> CheckQueue would loop
	branch->set_default(drop_command);

	move->add_output(wait_move);
	move->set_system_id(system::system_id_t::MOVE_COMMAND);

	wait_move->add_output(idle, activity::primer_wait);
	wait_move->add_output(branch, activity::primer_command_in_queue);

	gather_command->add_output(gather_wait);
	gather_command->set_system_id(system::system_id_t::GATHER_COMMAND);

	gather_wait->add_output(gather_active, activity::primer_wait);
	gather_wait->add_output(branch, activity::primer_command_in_queue);

	gather_active->add_output(gather_step, [](const time::time_t & /* time */, const std::shared_ptr<GameEntity> &entity) {
		return system::Gather::job_active(entity);
	});
	gather_active->set_default(idle);

	gather_step->add_output(gather_wait);
	gather_step->set_system_id(system::system_id_t::GATHER_STEP);

	drop_command->add_output(idle);
	drop_command->set_task_func([](const time::time_t &time, const std::shared_ptr<GameEntity> &entity) {
		auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
			entity->get_component(component::component_t::COMMANDQUEUE));
		auto command = queue->pop_command(time);
		if (command) {
			log::log(INFO << "Entity " << entity->get_id() << " ignores command "
			              << static_cast<int>(command->get_type()));
		}
	});

	// production (XR fork): villagers construct buildings
	// Branch -(build)-> BuildCommand -> BuildWait -(step done)-> BuildActive? -(yes)-> BuildStep -> BuildWait
	//                                                                         '-(no)-> Idle
	// BuildWait -(new command)-> Branch
	{
		auto build_command = std::make_shared<activity::TaskSystemNode>(20, "BuildCommand");
		auto build_wait = std::make_shared<activity::XorEventGate>(21);
		auto build_active = std::make_shared<activity::XorGate>(22);
		auto build_step = std::make_shared<activity::TaskSystemNode>(23, "BuildStep");

		branch->add_output(build_command, [](const time::time_t &time, const std::shared_ptr<GameEntity> &entity) {
			auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
				entity->get_component(component::component_t::COMMANDQUEUE));
			if (queue->get_queue().empty(time)) {
				return false;
			}
			return queue->get_queue().front(time)->get_type() == component::command::command_t::BUILD;
		});

		build_command->add_output(build_wait);
		build_command->set_system_id(system::system_id_t::BUILD_COMMAND);

		build_wait->add_output(build_active, activity::primer_wait);
		build_wait->add_output(branch, activity::primer_command_in_queue);

		build_active->add_output(build_step, [](const time::time_t & /* time */, const std::shared_ptr<GameEntity> &entity) {
			return system::Build::job_active(entity);
		});
		build_active->set_default(idle);

		build_step->add_output(build_wait);
		build_step->set_system_id(system::system_id_t::BUILD_STEP);
	}

	cached = std::make_shared<activity::Activity>(0, start, "xr.econ.gatherer");
	return cached;
}

std::optional<entity_id_t> pick_resource(const std::shared_ptr<GameState> &state,
                                         const coord::phys3 &ground_hit,
                                         const time::time_t &time) {
	auto origin_w = ground_hit.to_scene3().to_world_space();
	const auto &dir_w = renderer::camera::CAM_DIRECTION;
	const vec3 origin{origin_w.x(), origin_w.y(), origin_w.z()};
	const vec3 dir{dir_w.x(), dir_w.y(), dir_w.z()};

	std::optional<entity_id_t> best;
	double best_distance = std::numeric_limits<double>::max();
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (not entity->has_component(component::component_t::HARVESTABLE)) {
			continue;
		}
		auto harvestable = std::dynamic_pointer_cast<component::Harvestable>(
			entity->get_component(component::component_t::HARVESTABLE));
		if (harvestable->is_depleted()) {
			continue;
		}
		auto pos = std::dynamic_pointer_cast<component::Position>(
						entity->get_component(component::component_t::POSITION))
		               ->get_positions()
		               .get(time);
		auto base_w = pos.to_scene3().to_world_space();
		// vertical axis of the visible object: the sprite stands up from its base
		const double top = std::clamp(0.6 * harvestable->height, 0.4, 2.0);
		const vec3 base{base_w.x(), base_w.y(), base_w.z()};
		const vec3 tip{base_w.x(), base_w.y() + top, base_w.z()};
		auto distance = line_segment_distance(origin, dir, base, tip);
		const double pick_radius = std::clamp(harvestable->radius * 1.1, 0.45, 1.2);
		if (distance <= pick_radius and distance < best_distance) {
			best = id;
			best_distance = distance;
		}
	}
	return best;
}

bool can_gather_from(const std::shared_ptr<GameEntity> &gatherer,
                     const std::shared_ptr<GameEntity> &resource) {
	if (gatherer == nullptr or resource == nullptr
	    or not gatherer->has_component(component::component_t::GATHER)
	    or not resource->has_component(component::component_t::HARVESTABLE)) {
		return false;
	}
	auto gather = std::dynamic_pointer_cast<component::Gather>(
		gatherer->get_component(component::component_t::GATHER));
	auto harvestable = std::dynamic_pointer_cast<component::Harvestable>(
		resource->get_component(component::component_t::HARVESTABLE));
	return not harvestable->is_depleted() and gather->can_gather(harvestable->get_resource());
}

} // namespace openage::gamestate::econ
