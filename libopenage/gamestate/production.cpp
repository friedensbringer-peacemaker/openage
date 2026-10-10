// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "production.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "log/log.h"
#include "log/message.h"

#include "coord/phys.h"
#include "coord/scene.h"
#include "coord/tile.h"
#include "gamestate/api/ability.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/api/animation.h"
#include "gamestate/api/property.h"
#include "gamestate/api/types.h"
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/constructable.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/api/idle.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/api/move.h"
#include "gamestate/component/api/production_queue.h"
#include "gamestate/component/api/selectable.h"
#include "gamestate/component/internal/activity.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/build.h"
#include "gamestate/component/internal/commands/gather.h"
#include "gamestate/component/internal/commands/move.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/econ.h"
#include "gamestate/econ_math.h"
#include "gamestate/entity_factory.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/manager.h"
#include "gamestate/map.h"
#include "gamestate/player.h"
#include "gamestate/production_math.h"
#include "renderer/camera/definitions.h"


namespace openage::gamestate::prod {

namespace {

/// interval of the training queue update (seconds)
constexpr double TICK = 0.1;

/// identical status messages are logged at most this often (seconds)
constexpr double STATUS_LOG_INTERVAL = 2.0;

using ProductionComp = component::ProductionQueue;
using BuilderComp = component::Builder;
using ConstructableComp = component::Constructable;

template <typename T>
std::shared_ptr<T> component_of(const std::shared_ptr<GameEntity> &entity, component::component_t type) {
	if (entity == nullptr or not entity->has_component(type)) {
		return nullptr;
	}
	return std::dynamic_pointer_cast<T>(entity->get_component(type));
}

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

std::string tile_str(const coord::phys3 &pos) {
	return "(" + std::to_string(static_cast<int>(std::floor(pos.ne.to_double()))) + ", "
	       + std::to_string(static_cast<int>(std::floor(pos.se.to_double()))) + ")";
}

std::string cost_str(const resource_amounts_t &cost) {
	std::string out;
	for (size_t i = 0; i < RESOURCE_COUNT; ++i) {
		if (cost[i] > 0.0) {
			if (not out.empty()) {
				out += ", ";
			}
			out += to_string(static_cast<resource_t>(i));
			out += " -";
			out += std::to_string(static_cast<int>(std::lround(cost[i])));
		}
	}
	return out.empty() ? "free" : out;
}

/**
 * The converter writes sprites in game_entity/shared/ as "../shared/..." relative to
 * game_entity/generic/<unit>/ (same fix as in econ.cpp).
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

std::string object_name(const nyan::ValueHolder &value) {
	return std::dynamic_pointer_cast<nyan::ObjectValue>(value.get_ptr())->get_name();
}

/// resource amounts of a ResourceCost object
resource_amounts_t cost_of(const std::shared_ptr<nyan::View> &view, const nyan::Object &cost) {
	resource_amounts_t result{};
	for (const auto &amount_val : cost.get_set("ResourceCost.amount")) {
		auto amount = view->get_object(object_name(amount_val));
		auto type = resource_from_name(amount.get_object("ResourceAmount.type").get_name());
		if (type) {
			result[static_cast<size_t>(*type)] += static_cast<double>(amount.get_int("ResourceAmount.amount"));
		}
	}
	return result;
}

/// population space of a ProvideContingent ability
double population_of(const std::shared_ptr<nyan::View> &view, const nyan::Object &ability) {
	double result = 0.0;
	for (const auto &amount_val : ability.get_set("ProvideContingent.amount")) {
		auto amount = view->get_object(object_name(amount_val));
		auto type = short_name(amount.get_object("ResourceAmount.type").get_name());
		if (type == "PopulationSpace") {
			result += static_cast<double>(amount.get_int("ResourceAmount.amount"));
		}
	}
	return result;
}

/// hitbox radius of a game entity (Collision ability), default if none
double hitbox_radius(const std::shared_ptr<nyan::View> &view, const nyan::fqon_t &fqon, double fallback) {
	try {
		auto obj = view->get_object(fqon);
		for (const auto &ability_val : obj.get_set("GameEntity.abilities")) {
			auto ability = view->get_object(object_name(ability_val));
			if (ability.get_parents()[0] == "engine.ability.type.Collision") {
				return ability.get_object("Collision.hitbox").get_float("Hitbox.radius_x");
			}
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Production: no hitbox for " << fqon << ": " << err.what());
	}
	return fallback;
}

/// first animation of the first construction progress (the foundation)
std::string construct_animation(const std::shared_ptr<nyan::View> &view, const nyan::Object &ability) {
	std::string best;
	double best_left = std::numeric_limits<double>::max();
	for (const auto &progress_val : ability.get_set("Constructable.construction_progress")) {
		auto progress = view->get_object(object_name(progress_val));
		double left = progress.get_float("Progress.left_boundary");
		if (left >= best_left) {
			continue;
		}
		auto properties = progress.get<nyan::Dict>("Progress.properties");
		for (const auto &[key, value] : properties->get()) {
			if (short_name(object_name(key)) != "Animated") {
				continue;
			}
			auto animated = view->get_object(object_name(value));
			for (const auto &override_val : animated.get_set("Animated.overrides")) {
				auto override_obj = view->get_object(object_name(override_val));
				for (const auto &anim_val : override_obj.get_set("AnimationOverride.animations")) {
					auto anim = view->get_object(object_name(anim_val));
					auto path = fix_shared_path(api::APIAnimation::get_animation_path(anim));
					if (not path.empty()) {
						best = path;
						best_left = left;
					}
					break;
				}
				if (best_left == left) {
					break;
				}
			}
		}
	}
	return best;
}

/// placement mode Place among the placement modes of a creatable
bool is_placed(const std::shared_ptr<nyan::View> &view, const nyan::Object &creatable) {
	for (const auto &mode_val : creatable.get_set("CreatableGameEntity.placement_modes")) {
		auto mode = view->get_object(object_name(mode_val));
		if (mode.get_parents()[0] == "engine.util.placement_mode.type.Place") {
			return true;
		}
	}
	return false;
}

/// sort creatables in HUD order
template <typename T>
void sort_by_display(std::vector<T> &items) {
	std::stable_sort(items.begin(), items.end(), [](const T &a, const T &b) {
		return display_order(a.name) < display_order(b.name);
	});
}

/// path grid of a unit
std::optional<path::grid_id_t> grid_of(const std::shared_ptr<GameEntity> &entity,
                                       const std::shared_ptr<GameState> &state,
                                       const std::string &replace_last = {}) {
	auto move = component_of<component::Move>(entity, component::component_t::MOVE);
	if (move == nullptr) {
		return std::nullopt;
	}
	auto name = move->get_ability().get<nyan::ObjectValue>("Move.path_type")->get_name();
	if (not replace_last.empty()) {
		auto dot = name.rfind('.');
		name = (dot == std::string::npos ? std::string{} : name.substr(0, dot + 1)) + replace_last;
	}
	try {
		return state->get_map()->get_grid_id(name);
	}
	catch (std::exception &) {
		return std::nullopt;
	}
}

/// current health of a building (Live attribute), -1 if unknown
long long health_of(const std::shared_ptr<GameEntity> &building, const ConstructableComp &constructable, const time::time_t &time) {
	auto live = component_of<component::Live>(building, component::component_t::LIVE);
	if (live == nullptr or constructable.health_attribute.empty()) {
		return -1;
	}
	auto value = live->get_attribute(time, constructable.health_attribute);
	return value ? static_cast<long long>(*value) : -1;
}

/// HUD label of an entity
std::string label_of_entity(const std::shared_ptr<GameEntity> &entity) {
	return label_of(entity_name(entity));
}

/// true for units (they count for the population)
bool is_unit(const std::shared_ptr<GameEntity> &entity) {
	return entity->has_component(component::component_t::MOVE);
}

/// complete building (or one without construction)
bool is_complete(const std::shared_ptr<GameEntity> &entity) {
	auto constructable = component_of<ConstructableComp>(entity, component::component_t::CONSTRUCTABLE);
	return constructable == nullptr or constructable->is_complete();
}

/// population of a player
struct Population {
	size_t units = 0;
	size_t reserved = 0;
	double provided = 0.0;

	size_t used() const {
		return this->units + this->reserved;
	}
	size_t cap() const {
		return population_cap(this->provided);
	}
	size_t free() const {
		return this->cap() > this->used() ? this->cap() - this->used() : 0;
	}
};

std::unordered_map<player_id_t, Population> count_population(const std::shared_ptr<GameState> &state,
                                                             const time::time_t &time) {
	const auto &combat = state->get_combat();
	std::unordered_map<player_id_t, Population> result;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (not entity->has_component(component::component_t::OWNERSHIP)) {
			continue;
		}
		if (combat != nullptr and combat->is_dead(id)) {
			// dying units and burning buildings no longer count
			continue;
		}
		auto owner = owner_of(entity, time);
		if (is_unit(entity)) {
			result[owner].units += 1;
		}
		if (auto queue = component_of<ProductionComp>(entity, component::component_t::PRODUCTION_QUEUE)) {
			result[owner].reserved += queue->get_queue().reserved();
		}
		auto constructable = component_of<ConstructableComp>(entity, component::component_t::CONSTRUCTABLE);
		if (constructable != nullptr and constructable->is_complete()) {
			result[owner].provided += constructable->population;
		}
	}
	return result;
}

/**
 * Spawn a trained unit on a free tile next to its building; it walks to the
 * rally point if one is set.
 */
std::shared_ptr<GameEntity> spawn_unit(const std::shared_ptr<GameState> &state,
                                       const std::shared_ptr<openage::event::EventLoop> &loop,
                                       const std::shared_ptr<EntityFactory> &factory,
                                       const std::shared_ptr<GameEntity> &building,
                                       const ProductionComp &production,
                                       const std::string &fqon,
                                       const time::time_t &time) {
	auto owner = owner_of(building, time);
	auto entity = factory->add_game_entity(loop, state, owner, fqon);

	auto building_pos = position_of(building, time);
	auto constructable = component_of<ConstructableComp>(building, component::component_t::CONSTRUCTABLE);
	double radius = constructable ? constructable->radius : 1.0;
	auto ne = building_pos.ne.to_double();
	auto se = building_pos.se.to_double();
	// free side towards the rally point, else the front (south) of the building
	double from_ne = ne + radius + 1.0;
	double from_se = se + radius + 1.0;
	if (production.rally_point) {
		from_ne = production.rally_point->ne.to_double();
		from_se = production.rally_point->se.to_double();
	}
	auto map = state->get_map();
	auto grid = grid_of(entity, state);
	// tiles where units stand (new units do not appear on top of each other)
	std::set<std::pair<long, long>> unit_tiles;
	for (const auto &[id, other] : state->get_game_entities()) {
		if (other->has_component(component::component_t::MOVE)) {
			auto p = position_of(other, time);
			unit_tiles.insert({static_cast<long>(std::floor(p.ne.to_double())),
			                   static_cast<long>(std::floor(p.se.to_double()))});
		}
	}
	coord::phys3 pos = building_pos;
	bool placed = false;
	for (int ring = 0; ring < 3 and not placed; ++ring) {
		for (const auto &t : econ::approach_tiles(ne, se, radius + ring, from_ne, from_se)) {
			coord::tile tile{t.ne, t.se};
			if ((grid and not map->is_passable(*grid, tile)) or unit_tiles.contains({t.ne, t.se})) {
				continue;
			}
			pos = map->on_terrain(tile.to_phys3_center());
			placed = true;
			break;
		}
	}
	if (not placed) {
		// crowded: next to a unit is better than inside the building
		for (const auto &t : econ::approach_tiles(ne, se, radius, from_ne, from_se)) {
			coord::tile tile{t.ne, t.se};
			if (not grid or map->is_passable(*grid, tile)) {
				pos = map->on_terrain(tile.to_phys3_center());
				break;
			}
		}
	}

	auto entity_pos = std::dynamic_pointer_cast<component::Position>(
		entity->get_component(component::component_t::POSITION));
	entity_pos->set_position(time, pos);
	entity_pos->set_angle(time, coord::phys_angle_t::from_int(315));

	auto entity_owner = std::dynamic_pointer_cast<component::Ownership>(
		entity->get_component(component::component_t::OWNERSHIP));
	entity_owner->set_owner(time, owner);

	auto activity = std::dynamic_pointer_cast<component::Activity>(
		entity->get_component(component::component_t::ACTIVITY));
	activity->init(time);
	entity->get_manager()->run_activity_system(time);

	state->add_game_entity(entity);

	if (production.rally_point and entity->has_component(component::component_t::MOVE)) {
		auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
			entity->get_component(component::component_t::COMMANDQUEUE));
		using rally_t = ProductionComp::rally_t;
		const auto kind = production.rally_target ? production.rally_target->kind : rally_t::GROUND;
		auto target = production.rally_target ? find_entity(state, production.rally_target->entity) : nullptr;
		bool done = false;
		if (kind == rally_t::RESOURCE and entity->has_component(component::component_t::GATHER)) {
			// the rally resource, or the next spot of its type near the rally point if it is empty
			std::shared_ptr<GameEntity> spot = econ::can_gather_from(entity, target) ? target : nullptr;
			if (spot == nullptr) {
				double best = 16.0 * 16.0;
				const auto &rally = *production.rally_point;
				for (const auto &[id, candidate] : state->get_game_entities()) {
					auto h = component_of<component::Harvestable>(candidate, component::component_t::HARVESTABLE);
					if (h == nullptr or h->is_depleted() or h->get_resource() != production.rally_target->resource
					    or not econ::can_gather_from(entity, candidate)) {
						continue;
					}
					auto p = position_of(candidate, time);
					double dn = p.ne.to_double() - rally.ne.to_double();
					double ds = p.se.to_double() - rally.se.to_double();
					if (dn * dn + ds * ds < best) {
						best = dn * dn + ds * ds;
						spot = candidate;
					}
				}
			}
			if (spot != nullptr) {
				queue->add_command(time, std::make_shared<component::command::GatherCommand>(spot->get_id()));
				log::log(INFO << "Production: new unit " << entity->get_id() << " gathers "
				              << to_string(production.rally_target->resource) << " at entity " << spot->get_id()
				              << " (rally point)");
				done = true;
			}
		}
		else if (kind == rally_t::ENEMY and target != nullptr and state->get_combat() != nullptr
		         and state->get_combat()->order_attack(state, entity->get_id(), target->get_id(), time)) {
			log::log(INFO << "Production: new unit " << entity->get_id() << " attacks entity " << target->get_id()
			              << " (rally point)");
			done = true;
		}
		else if (kind == rally_t::ENTITY and target != nullptr) {
			// walk to the entity where it is now
			queue->add_command(time, std::make_shared<component::command::MoveCommand>(position_of(target, time)));
			log::log(INFO << "Production: new unit " << entity->get_id() << " walks to entity " << target->get_id()
			              << " (rally point)");
			done = true;
		}
		if (not done) {
			queue->add_command(time, std::make_shared<component::command::MoveCommand>(*production.rally_point));
		}
	}
	return entity;
}

} // namespace


void init_components(const std::shared_ptr<openage::event::EventLoop> &loop,
                     const std::shared_ptr<nyan::View> &owner_db_view,
                     const std::shared_ptr<GameEntity> &entity,
                     const nyan::fqon_t &nyan_entity) {
	auto nyan_obj = owner_db_view->get_object(nyan_entity);
	nyan::set_t abilities = nyan_obj.get_set("GameEntity.abilities");

	std::optional<nyan::Object> create_ability;
	std::optional<nyan::Object> constructable_ability;
	std::vector<ProductionComp::Creatable> units;
	std::vector<BuilderComp::Buildable> buildings;
	// build times per construct type ("House" from ...construct_type.types.HouseConstruct)
	std::unordered_map<std::string, double> build_times;
	std::string build_animation;
	double population = 0.0;
	double radius = 0.5;
	nyan::fqon_t health_attribute;
	int64_t max_health = 0;

	for (const auto &ability_val : abilities) {
		auto ability_fqon = object_name(ability_val);
		auto ability_obj = owner_db_view->get_object(ability_fqon);
		auto ability_parent = ability_obj.get_parents()[0];

		try {
			if (ability_parent == "engine.ability.type.Create") {
				create_ability = ability_obj;
				for (const auto &creatable_val : ability_obj.get_set("Create.creatables")) {
					auto creatable = owner_db_view->get_object(object_name(creatable_val));
					auto fqon = creatable.get_object("CreatableGameEntity.game_entity").get_name();
					auto name = short_name(fqon);
					const auto *entry = known(name);
					if (entry == nullptr) {
						// not offered yet (ages, technologies, unique units)
						continue;
					}
					auto cost = cost_of(owner_db_view, creatable.get_object("CreatableGameEntity.cost"));
					if (entry->building and is_placed(owner_db_view, creatable)) {
						BuilderComp::Buildable buildable;
						buildable.name = name;
						buildable.fqon = fqon;
						buildable.cost = cost;
						buildable.radius = hitbox_radius(owner_db_view, fqon, 1.0);
						buildings.push_back(buildable);
					}
					else if (not entry->building) {
						ProductionComp::Creatable unit;
						unit.name = name;
						unit.fqon = fqon;
						unit.cost = cost;
						auto time = creatable.get_float("CreatableGameEntity.creation_time");
						unit.time = time > 0.0 ? time : DEFAULT_TRAIN_TIME;
						units.push_back(unit);
					}
				}
			}
			else if (ability_parent == "engine.ability.type.ApplyContinuousEffect") {
				// Construct: progress effects per construct type
				bool constructs = false;
				for (const auto &effect_val : ability_obj.get_set("ApplyContinuousEffect.effects")) {
					auto effect = owner_db_view->get_object(object_name(effect_val));
					if (short_name(effect.get_parents()[0]) != "TimeRelativeProgressIncrease") {
						continue;
					}
					auto type = short_name(effect.get_object("TimeRelativeProgressChange.type").get_name());
					const std::string suffix = "Construct";
					if (type.size() <= suffix.size()
					    or type.compare(type.size() - suffix.size(), suffix.size(), suffix) != 0) {
						continue;
					}
					build_times[type.substr(0, type.size() - suffix.size())] =
						effect.get_float("TimeRelativeProgressChange.total_change_time");
					constructs = true;
				}
				if (constructs) {
					build_animation = animation_of(ability_obj);
				}
			}
			else if (ability_parent == "engine.ability.type.Constructable") {
				constructable_ability = ability_obj;
			}
			else if (ability_parent == "engine.ability.type.ProvideContingent") {
				population += population_of(owner_db_view, ability_obj);
			}
			else if (ability_parent == "engine.ability.type.Collision") {
				radius = ability_obj.get_object("Collision.hitbox").get_float("Hitbox.radius_x");
			}
			else if (ability_parent == "engine.ability.type.Live") {
				for (const auto &setting_val : ability_obj.get_set("Live.attributes")) {
					auto setting = owner_db_view->get_object(object_name(setting_val));
					auto attribute = setting.get_object("AttributeSetting.attribute").get_name();
					if (short_name(attribute) == "Health") {
						health_attribute = attribute;
						max_health = setting.get_int("AttributeSetting.max_value");
					}
				}
			}
		}
		catch (std::exception &err) {
			log::log(WARN << "Production: ability " << ability_fqon << " of " << nyan_entity
			              << " not usable: " << err.what());
		}
	}

	if (create_ability and not units.empty()) {
		sort_by_display(units);
		entity->add_component(std::make_shared<ProductionComp>(loop, *create_ability, std::move(units)));
	}
	if (create_ability and not buildings.empty()) {
		for (auto &building : buildings) {
			auto it = build_times.find(building.name);
			if (it != build_times.end() and it->second > 0.0) {
				building.time = it->second;
			}
		}
		sort_by_display(buildings);
		auto builder = std::make_shared<BuilderComp>(loop, *create_ability, std::move(buildings));
		builder->animation = build_animation;
		entity->add_component(builder);
	}
	if (constructable_ability) {
		auto constructable = std::make_shared<ConstructableComp>(loop, *constructable_ability);
		constructable->population = population;
		constructable->radius = radius;
		constructable->health_attribute = health_attribute;
		constructable->max_health = max_health;
		try {
			constructable->construct_animation = construct_animation(owner_db_view, *constructable_ability);
		}
		catch (std::exception &err) {
			log::log(WARN << "Production: construction animation of " << nyan_entity
			              << " not usable: " << err.what());
		}
		entity->add_component(constructable);
	}
}

std::optional<entity_id_t> pick_foundation(const std::shared_ptr<GameState> &state,
                                           const coord::phys3 &ground_hit,
                                           player_id_t player,
                                           const time::time_t &time) {
	auto origin_w = ground_hit.to_scene3().to_world_space();
	const auto &dir_w = renderer::camera::CAM_DIRECTION;
	const econ::vec3 origin{origin_w.x(), origin_w.y(), origin_w.z()};
	const econ::vec3 dir{dir_w.x(), dir_w.y(), dir_w.z()};

	std::optional<entity_id_t> best;
	double best_distance = std::numeric_limits<double>::max();
	for (const auto &[id, entity] : state->get_game_entities()) {
		auto constructable = component_of<ConstructableComp>(entity, component::component_t::CONSTRUCTABLE);
		if (constructable == nullptr or constructable->is_complete() or owner_of(entity, time) != player) {
			continue;
		}
		auto base_w = position_of(entity, time).to_scene3().to_world_space();
		const econ::vec3 base{base_w.x(), base_w.y(), base_w.z()};
		const econ::vec3 tip{base_w.x(), base_w.y() + 0.5, base_w.z()};
		auto distance = econ::line_segment_distance(origin, dir, base, tip);
		if (distance <= constructable->radius + 0.3 and distance < best_distance) {
			best = id;
			best_distance = distance;
		}
	}
	return best;
}

bool can_build(const std::shared_ptr<GameEntity> &builder,
               const std::shared_ptr<GameEntity> &foundation,
               const time::time_t &time) {
	if (builder == nullptr or foundation == nullptr
	    or not builder->has_component(component::component_t::BUILDER)) {
		return false;
	}
	auto constructable = component_of<ConstructableComp>(foundation, component::component_t::CONSTRUCTABLE);
	return constructable != nullptr and not constructable->is_complete()
	       and owner_of(builder, time) == owner_of(foundation, time);
}

void complete_building(const std::shared_ptr<GameEntity> &building,
                       const time::time_t &time) {
	auto constructable = component_of<ConstructableComp>(building, component::component_t::CONSTRUCTABLE);
	if (constructable == nullptr) {
		return;
	}
	if (constructable->pending_drop_site) {
		building->add_component(constructable->pending_drop_site);
		constructable->pending_drop_site = nullptr;
	}
	if (auto idle = component_of<component::Idle>(building, component::component_t::IDLE)) {
		auto animation = animation_of(idle->get_ability());
		if (not animation.empty()) {
			building->render_update(time, animation);
		}
	}
	log::log(INFO << "Production: " << entity_name(building) << " (entity " << building->get_id()
	              << ") at tile " << tile_str(position_of(building, time)) << " complete after "
	              << constructable->get_build_time() << " s of work, provides "
	              << constructable->population << " population, health "
	              << health_of(building, *constructable, time) << "/"
	              << constructable->max_health);
}

void update_building_health(const std::shared_ptr<GameState> &state,
                            const std::shared_ptr<GameEntity> &building,
                            double previous_progress,
                            const time::time_t &time) {
	auto constructable = component_of<ConstructableComp>(building, component::component_t::CONSTRUCTABLE);
	auto live = component_of<component::Live>(building, component::component_t::LIVE);
	if (constructable == nullptr or live == nullptr or constructable->health_attribute.empty()
	    or constructable->max_health <= 0) {
		return;
	}
	const auto max = static_cast<long long>(constructable->max_health);
	long long health = construction_health(max, constructable->get_progress());
	if (previous_progress >= 0.0) {
		auto current = live->get_attribute(time, constructable->health_attribute);
		long long gain = health - construction_health(max, previous_progress);
		long long base = current ? static_cast<long long>(*current) : construction_health(max, previous_progress);
		health = std::clamp(base + gain, 1LL, max);
	}
	live->set_attribute(time, constructable->health_attribute, health);
	if (auto combat = state->get_combat()) {
		combat->health_changed(building->get_id(), health);
	}
}

std::string entity_name(const std::shared_ptr<GameEntity> &entity) {
	for (auto type : {component::component_t::IDLE,
	                  component::component_t::SELECTABLE,
	                  component::component_t::MOVE,
	                  component::component_t::LIVE,
	                  component::component_t::CONSTRUCTABLE}) {
		if (not entity->has_component(type)) {
			continue;
		}
		auto api = std::dynamic_pointer_cast<component::APIComponent>(entity->get_component(type));
		if (api == nullptr) {
			continue;
		}
		const auto &ability = api->get_ability().get_name();
		auto dot = ability.rfind('.');
		if (dot != std::string::npos) {
			return short_name(ability.substr(0, dot));
		}
	}
	return {};
}


// --- Production: thread-safe interface ---

Snapshot Production::snapshot() const {
	std::lock_guard<std::mutex> lock{this->mutex};
	auto result = this->current;
	result.placement = this->placement;
	return result;
}

void Production::set_player(player_id_t player) {
	std::lock_guard<std::mutex> lock{this->mutex};
	this->player = player;
	this->dirty = true;
}

void Production::set_selection(const std::vector<entity_id_t> &ids) {
	std::lock_guard<std::mutex> lock{this->mutex};
	this->selection = ids;
	this->dirty = true;
}

void Production::command(int code) {
	const auto *entry = known(code);
	if (entry == nullptr) {
		this->set_status("Unbekannter Befehl", status_t::warn);
		return;
	}
	if (entry->building) {
		this->start_placement(entry->name);
	}
	else {
		this->train(entry->name);
	}
}

void Production::train(const std::string &id) {
	Request request;
	request.kind = Request::kind_t::TRAIN;
	request.id = id;
	this->push(std::move(request));
}

void Production::train_in(entity_id_t building, const std::string &id) {
	Request request;
	request.kind = Request::kind_t::TRAIN_IN;
	request.id = id;
	request.building = building;
	this->push(std::move(request));
}

void Production::cancel_training() {
	Request request;
	request.kind = Request::kind_t::CANCEL;
	this->push(std::move(request));
}

void Production::cancel_training_at(size_t index) {
	Request request;
	request.kind = Request::kind_t::CANCEL_AT;
	request.index = index;
	this->push(std::move(request));
}

void Production::notify(const std::string &text, status_t kind) {
	this->set_status(text, kind);
}

void Production::track_foundation(entity_id_t building) {
	// simulation thread (save games are restored before the loop runs)
	this->foundations.insert(building);
}

bool Production::start_placement(const std::string &id) {
	std::lock_guard<std::mutex> lock{this->mutex};
	std::vector<const Option *> buildings;
	for (const auto &option : this->current.options) {
		if (option.building) {
			buildings.push_back(&option);
		}
	}
	if (buildings.empty()) {
		this->set_status_locked("Keine Dorfbewohner ausgewählt", status_t::warn);
		return not this->placement.empty();
	}

	const Option *chosen = nullptr;
	if (id.empty()) {
		// next building after the current one (cycles)
		size_t next = 0;
		for (size_t i = 0; i < buildings.size(); ++i) {
			if (buildings[i]->id == this->placement) {
				next = i + 1;
			}
		}
		chosen = buildings[next % buildings.size()];
	}
	else {
		for (const auto *option : buildings) {
			if (option->id == id) {
				chosen = option;
			}
		}
		if (chosen == nullptr) {
			this->set_status_locked("Die Auswahl kann " + label_of(id) + " nicht bauen", status_t::warn);
			return not this->placement.empty();
		}
	}
	if (not chosen->available) {
		this->set_status_locked(chosen->reason, status_t::warn);
		return not this->placement.empty();
	}
	this->placement = chosen->id;
	this->set_status_locked(chosen->label + " platzieren (Klick setzt, Rechtsklick/Esc bricht ab)", status_t::info);
	log::log(INFO << "Production: placement mode " << chosen->id);
	return true;
}

void Production::cancel_placement() {
	std::lock_guard<std::mutex> lock{this->mutex};
	if (this->placement.empty()) {
		return;
	}
	log::log(INFO << "Production: placement of " << this->placement << " cancelled");
	this->placement.clear();
	this->set_status_locked("Bauen abgebrochen", status_t::info);
}

bool Production::placement_active() const {
	std::lock_guard<std::mutex> lock{this->mutex};
	return not this->placement.empty();
}

bool Production::place_at(const coord::phys3 &ground_hit) {
	std::lock_guard<std::mutex> lock{this->mutex};
	if (this->placement.empty()) {
		return false;
	}
	Request request;
	request.kind = Request::kind_t::PLACE;
	request.id = this->placement;
	request.pos = ground_hit;
	this->requests.push_back(std::move(request));
	this->placement.clear();
	this->dirty = true;
	return true;
}

void Production::place(const std::string &id, const coord::phys3 &ground_hit) {
	Request request;
	request.kind = Request::kind_t::PLACE;
	request.id = id;
	request.pos = ground_hit;
	this->push(std::move(request));
}

// ---- ai (XR fork)
void Production::train_for(player_id_t player, entity_id_t building, const std::string &id) {
	Request request;
	request.kind = Request::kind_t::TRAIN_IN;
	request.id = id;
	request.building = building;
	request.for_player = player;
	this->push(std::move(request));
}

void Production::place_for(player_id_t player,
                           const std::vector<entity_id_t> &builders,
                           const std::string &id,
                           const coord::phys3 &ground_hit) {
	Request request;
	request.kind = Request::kind_t::PLACE;
	request.id = id;
	request.pos = ground_hit;
	request.for_player = player;
	request.builders = builders;
	this->push(std::move(request));
}

PlayerPopulation population_of(const std::shared_ptr<GameState> &state,
                               player_id_t player,
                               const time::time_t &time) {
	auto all = count_population(state, time);
	auto it = all.find(player);
	if (it == all.end()) {
		return {};
	}
	return {it->second.used(), it->second.cap()};
}

size_t queued_in(const std::shared_ptr<GameEntity> &building) {
	auto production = component_of<ProductionComp>(building, component::component_t::PRODUCTION_QUEUE);
	return production == nullptr ? 0 : production->get_queue().size();
}

bool is_finished(const std::shared_ptr<GameEntity> &building) {
	return is_complete(building);
}

bool set_rally_point(const std::shared_ptr<GameState> &state,
                     const std::shared_ptr<GameEntity> &building,
                     const coord::phys3 &ground,
                     const std::shared_ptr<GameEntity> &target_entity,
                     const time::time_t &time) {
	auto production = component_of<ProductionComp>(building, component::component_t::PRODUCTION_QUEUE);
	if (production == nullptr) {
		return false;
	}
	using rally_t = ProductionComp::rally_t;
	ProductionComp::RallyTarget rally;
	coord::phys3 point = ground;
	std::string what = "ground";
	if (target_entity != nullptr and target_entity != building
	    and target_entity->has_component(component::component_t::POSITION)) {
		point = position_of(target_entity, time);
		rally.entity = target_entity->get_id();
		auto harvestable = component_of<component::Harvestable>(target_entity, component::component_t::HARVESTABLE);
		const auto combat = state->get_combat();
		const bool own = target_entity->has_component(component::component_t::OWNERSHIP)
		                 and owner_of(target_entity, time) == owner_of(building, time);
		if (harvestable != nullptr and not harvestable->is_depleted()) {
			rally.kind = rally_t::RESOURCE;
			rally.resource = harvestable->get_resource();
			what = std::string{"resource "} + to_string(rally.resource);
		}
		else if (own) {
			rally.kind = rally_t::ENTITY;
			what = "own " + entity_name(target_entity);
		}
		else {
			rally.kind = rally_t::ENEMY;
			what = "enemy " + entity_name(target_entity);
		}
	}
	production->rally_point = point;
	if (rally.kind == rally_t::GROUND) {
		production->rally_target.reset();
	}
	else {
		production->rally_target = rally;
	}
	log::log(INFO << "Production: rally point set on " << what
	              << (rally.kind == rally_t::GROUND ? std::string{} : " (entity " + std::to_string(rally.entity) + ")")
	              << " at tile " << tile_str(point) << " for " << entity_name(building) << " (entity "
	              << building->get_id() << ")");
	return true;
}
// ---- end ai (XR fork)

void Production::push(Request &&request) {
	std::lock_guard<std::mutex> lock{this->mutex};
	this->requests.push_back(std::move(request));
	this->dirty = true;
}

void Production::set_status(const std::string &text, status_t kind) {
	std::lock_guard<std::mutex> lock{this->mutex};
	this->set_status_locked(text, kind);
}

void Production::set_status_locked(const std::string &text, status_t kind) {
	this->current.status = text;
	this->current.status_kind = kind;
	this->current.status_seq += 1;
	this->current.status_time = this->now_seconds;
	if (text != this->last_logged_status or this->now_seconds >= this->last_status_log + STATUS_LOG_INTERVAL
	    or this->now_seconds < this->last_status_log) {
		this->last_logged_status = text;
		this->last_status_log = this->now_seconds;
		log::log(INFO << "Production status (" << (kind == status_t::warn ? "warn" : kind == status_t::good ? "good" : "info")
		              << "): " << text);
	}
}


// --- Production: simulation thread ---

void Production::update(const std::shared_ptr<GameState> &state,
                        const std::shared_ptr<openage::event::EventLoop> &loop,
                        const std::shared_ptr<EntityFactory> &factory,
                        const time::time_t &now) {
	const double t = now.to_double();
	std::vector<Request> todo;
	std::vector<entity_id_t> selected;
	player_id_t player_id;
	bool was_dirty;
	{
		std::lock_guard<std::mutex> lock{this->mutex};
		this->now_seconds = t;
		was_dirty = this->dirty;
		this->dirty = false;
		todo.swap(this->requests);
		selected = this->selection;
		player_id = this->player;
	}
	// XR fork: ghost of the placement mode (own rate, independent of the tick)
	this->update_placement_preview(state, now);
	const bool tick = t >= this->last_tick + TICK or t < this->last_tick;
	if (not tick and not was_dirty) {
		return;
	}
	if (tick) {
		this->last_tick = t;
	}
	if (state == nullptr or state->get_map() == nullptr) {
		return;
	}
	const auto &entities = state->get_game_entities();

	// own entities of the selection (others are ignored)
	std::vector<std::shared_ptr<GameEntity>> own;
	for (auto id : selected) {
		auto entity = find_entity(state, id);
		if (entity != nullptr and entity->has_component(component::component_t::OWNERSHIP)
		    and owner_of(entity, now) == player_id
		    and not (state->get_combat() != nullptr and state->get_combat()->is_dead(id))) {
			own.push_back(entity);
		}
	}

	auto population = count_population(state, now);

	// --- requests ---
	for (const auto &request : todo) {
		// ai (XR fork): requests of a computer opponent leave the HUD status alone
		auto status = [&](const std::string &text, status_t kind) {
			if (not request.for_player) {
				this->set_status(text, kind);
			}
		};
		switch (request.kind) {
		case Request::kind_t::TRAIN:
		case Request::kind_t::TRAIN_IN: {
			// candidate buildings: complete, own, can train the unit; the shortest queue wins
			std::vector<std::shared_ptr<GameEntity>> candidates;
			if (request.kind == Request::kind_t::TRAIN_IN) {
				auto building = find_entity(state, request.building);
				// ai (XR fork): a computer opponent trains in its own buildings only
				if (building != nullptr
				    and (not request.for_player or owner_of(building, now) == *request.for_player)) {
					candidates.push_back(building);
				}
			}
			else {
				candidates = own;
			}
			std::shared_ptr<GameEntity> best;
			const ProductionComp::Creatable *creatable = nullptr;
			size_t best_size = std::numeric_limits<size_t>::max();
			for (const auto &entity : candidates) {
				auto production = component_of<ProductionComp>(entity, component::component_t::PRODUCTION_QUEUE);
				if (production == nullptr or not is_complete(entity)) {
					continue;
				}
				const auto *c = request.id.empty() ? &production->get_creatables().front()
				                                   : production->find(request.id);
				if (c == nullptr) {
					continue;
				}
				if (production->get_queue().size() < best_size) {
					best = entity;
					creatable = c;
					best_size = production->get_queue().size();
				}
			}
			if (best == nullptr) {
				status(request.id.empty() ? "Die Auswahl kann nichts ausbilden"
				                                    : "Die Auswahl kann " + label_of(request.id) + " nicht ausbilden",
				                 status_t::warn);
				break;
			}
			auto production = component_of<ProductionComp>(best, component::component_t::PRODUCTION_QUEUE);
			auto owner = owner_of(best, now);
			if (production->get_queue().size() >= MAX_QUEUE) {
				status("Warteschlange voll", status_t::warn);
				break;
			}
			auto &stock = state->get_player(owner)->get_resources();
			if (not stock.spend(creatable->cost)) {
				auto missing = missing_resource(stock.get(), creatable->cost);
				status(missing ? missing_message(*missing) : "Nicht genug Rohstoffe", status_t::warn);
				log::log(INFO << "Production: player " << owner << " cannot afford " << creatable->name);
				break;
			}
			TrainQueue::Item item;
			item.name = creatable->name;
			item.fqon = creatable->fqon;
			item.cost = creatable->cost;
			item.time = creatable->time;
			production->get_queue().push(item);
			log::log(INFO << "Production: player " << owner << " queues " << creatable->name << " in "
			              << entity_name(best) << " (entity " << best->get_id() << ", queue "
			              << production->get_queue().size() << "/" << MAX_QUEUE << ", "
			              << creatable->time << " s), " << cost_str(creatable->cost));
			status(label_of(creatable->name) + " wird ausgebildet", status_t::info);
		} break;

		case Request::kind_t::CANCEL:
		case Request::kind_t::CANCEL_AT: {
			bool done = false;
			for (const auto &entity : own) {
				auto production = component_of<ProductionComp>(entity, component::component_t::PRODUCTION_QUEUE);
				if (production == nullptr or production->get_queue().size() == 0) {
					continue;
				}
				auto item = request.kind == Request::kind_t::CANCEL_AT
				                ? production->get_queue().cancel_at(request.index)
				                : production->get_queue().cancel_last();
				if (not item) {
					continue;
				}
				auto owner = owner_of(entity, now);
				auto &stock = state->get_player(owner)->get_resources();
				for (size_t i = 0; i < RESOURCE_COUNT; ++i) {
					stock.add(static_cast<resource_t>(i), item->cost[i]);
				}
				log::log(INFO << "Production: player " << owner << " cancels " << item->name << " in entity "
				              << entity->get_id() << ", refunded");
				status(label_of(item->name) + " abgebrochen", status_t::info);
				done = true;
				break;
			}
			if (not done) {
				status("Nichts in Ausbildung", status_t::info);
			}
		} break;

		case Request::kind_t::PLACE: {
			std::vector<std::shared_ptr<GameEntity>> builders;
			const BuilderComp::Buildable *buildable = nullptr;
			// ai (XR fork): builders of a computer opponent instead of the HUD selection
			std::vector<std::shared_ptr<GameEntity>> ai_builders;
			if (request.for_player) {
				for (auto id : request.builders) {
					auto entity = find_entity(state, id);
					if (entity != nullptr and entity->has_component(component::component_t::OWNERSHIP)
					    and owner_of(entity, now) == *request.for_player
					    and not (state->get_combat() != nullptr and state->get_combat()->is_dead(id))) {
						ai_builders.push_back(entity);
					}
				}
			}
			for (const auto &entity : request.for_player ? ai_builders : own) {
				auto builder = component_of<BuilderComp>(entity, component::component_t::BUILDER);
				if (builder == nullptr) {
					continue;
				}
				const auto *b = builder->find(request.id);
				if (b == nullptr) {
					continue;
				}
				if (buildable == nullptr) {
					buildable = b;
				}
				builders.push_back(entity);
			}
			if (buildable == nullptr) {
				status("Keine Dorfbewohner ausgewählt", status_t::warn);
				break;
			}
			auto map = state->get_map();
			auto hit = map->pick_terrain(request.pos);
			auto [ane, ase] = snap_anchor(hit.ne.to_double(), hit.se.to_double(), buildable->radius);
			auto tiles = building_tiles(ane, ase, buildable->radius);

			// tiles where units stand
			std::set<std::pair<long, long>> unit_tiles;
			for (const auto &[id, entity] : entities) {
				if (is_unit(entity)) {
					auto p = position_of(entity, now);
					unit_tiles.insert({static_cast<long>(std::floor(p.ne.to_double())),
					                   static_cast<long>(std::floor(p.se.to_double()))});
				}
			}
			auto land = grid_of(builders.front(), state);
			auto water = grid_of(builders.front(), state, "Water");
			const auto size = map->get_size();
			auto result = check_placement(tiles, [&](const econ::tile_pos &t) {
				if (t.ne < 0 or t.se < 0 or t.ne >= static_cast<long>(size[0]) or t.se >= static_cast<long>(size[1])) {
					return tile_state_t::OUTSIDE;
				}
				coord::tile tile{t.ne, t.se};
				if (land and not map->is_passable(*land, tile)) {
					if (water and map->is_passable(*water, tile)) {
						return tile_state_t::WATER;
					}
					return tile_state_t::BLOCKED;
				}
				// landscapes: ice and fords are land paths, but frozen/shallow water
				if (not terrain_buildable(map->terrain_name(tile))) {
					return tile_state_t::WATER;
				}
				if (unit_tiles.contains({t.ne, t.se})) {
					return tile_state_t::OCCUPIED;
				}
				return tile_state_t::FREE;
			});
			coord::phys3 anchor{coord::phys_t{ane}, coord::phys_t{ase}, coord::phys_t{0.0}};
			if (result != placement_t::OK) {
				status(placement_message(result), status_t::warn);
				log::log(INFO << "Production: " << request.id << " at tile " << tile_str(anchor)
				              << " rejected: " << placement_message(result));
				break;
			}
			auto owner = owner_of(builders.front(), now);
			auto &stock = state->get_player(owner)->get_resources();
			if (not stock.spend(buildable->cost)) {
				auto missing = missing_resource(stock.get(), buildable->cost);
				status(missing ? missing_message(*missing) : "Nicht genug Rohstoffe", status_t::warn);
				log::log(INFO << "Production: player " << owner << " cannot afford " << buildable->name);
				break;
			}

			// foundation
			auto building = factory->add_game_entity(loop, state, owner, buildable->fqon);
			auto building_pos = std::dynamic_pointer_cast<component::Position>(
				building->get_component(component::component_t::POSITION));
			building_pos->set_position(now, map->on_terrain(anchor));
			building_pos->set_angle(now, coord::phys_angle_t::from_int(315));
			auto building_owner = std::dynamic_pointer_cast<component::Ownership>(
				building->get_component(component::component_t::OWNERSHIP));
			building_owner->set_owner(now, owner);
			auto activity = std::dynamic_pointer_cast<component::Activity>(
				building->get_component(component::component_t::ACTIVITY));
			activity->init(now);
			building->get_manager()->run_activity_system(now);
			state->add_game_entity(building);

			auto constructable = component_of<ConstructableComp>(building, component::component_t::CONSTRUCTABLE);
			if (constructable == nullptr) {
				// no construction in the data: complete at once
				log::log(WARN << "Production: " << buildable->name << " has no Constructable, placed complete");
			}
			else {
				constructable->start_foundation(buildable->time);
				update_building_health(state, building, -1.0, now);
				if (building->has_component(component::component_t::DROP_SITE)) {
					constructable->pending_drop_site = building->get_component(component::component_t::DROP_SITE);
					building->remove_component(component::component_t::DROP_SITE);
				}
				if (not constructable->construct_animation.empty()) {
					building->render_update(now, constructable->construct_animation);
				}
				this->foundations.insert(building->get_id());
			}
			for (const auto &t : tiles) {
				map->block_tile(coord::tile{t.ne, t.se}, now);
			}

			// the selected villagers start building
			for (const auto &builder : builders) {
				auto queue = std::dynamic_pointer_cast<component::CommandQueue>(
					builder->get_component(component::component_t::COMMANDQUEUE));
				queue->add_command(now, std::make_shared<component::command::BuildCommand>(building->get_id()));
			}
			log::log(INFO << "Production: player " << owner << " places " << buildable->name << " (entity "
			              << building->get_id() << ") at tile " << tile_str(anchor) << ", " << tiles.size()
			              << " tiles, " << builders.size() << " builders, build time " << buildable->time
			              << " s, " << cost_str(buildable->cost));
			status(label_of(buildable->name) + " wird gebaut", status_t::info);
		} break;

		default:
			break;
		}
	}
	if (not todo.empty()) {
		population = count_population(state, now);
	}

	// --- training queues and finished foundations ---
	if (tick) {
		std::unordered_map<player_id_t, size_t> free;
		for (const auto &[owner, pop] : population) {
			free[owner] = pop.free();
		}
		std::vector<std::pair<std::shared_ptr<GameEntity>, std::shared_ptr<ProductionComp>>> producers;
		for (const auto &[id, entity] : entities) {
			auto production = component_of<ProductionComp>(entity, component::component_t::PRODUCTION_QUEUE);
			if (production != nullptr and production->get_queue().size() > 0 and is_complete(entity)) {
				producers.emplace_back(entity, production);
			}
		}
		// deterministic order (the entity map is unordered)
		std::sort(producers.begin(), producers.end(), [](const auto &a, const auto &b) {
			return a.first->get_id() < b.first->get_id();
		});
		for (auto &[entity, production] : producers) {
			auto owner = owner_of(entity, now);
			auto &slot_list = free[owner];
			auto done = production->get_queue().advance(t, slot_list);
			for (const auto &item : done) {
				auto unit = spawn_unit(state, loop, factory, entity, *production, item.fqon, now);
				log::log(INFO << "Production: " << item.name << " (entity " << unit->get_id() << ") trained in "
				              << entity_name(entity) << " (entity " << entity->get_id() << ") at t=" << t
				              << " s, spawned at tile " << tile_str(position_of(unit, now)) << ", queue left "
				              << production->get_queue().size());
				if (owner == player_id) {
					this->set_status(label_of(item.name) + " bereit", status_t::good);
				}
			}
			bool waits = production->get_queue().is_waiting();
			bool was_waiting = this->waiting.contains(entity->get_id());
			if (waits and not was_waiting) {
				this->waiting.insert(entity->get_id());
				log::log(INFO << "Production: " << entity_name(entity) << " (entity " << entity->get_id()
				              << ") waits: population limit " << population[owner].cap());
				if (owner == player_id) {
					this->set_status("Bevölkerungsgrenze erreicht – Haus bauen", status_t::warn);
				}
			}
			else if (not waits and was_waiting) {
				this->waiting.erase(entity->get_id());
			}
		}
		for (auto it = this->foundations.begin(); it != this->foundations.end();) {
			auto building = find_entity(state, *it);
			if (building == nullptr) {
				it = this->foundations.erase(it);
				continue;
			}
			if (is_complete(building)) {
				if (owner_of(building, now) == player_id) {
					this->set_status(label_of(entity_name(building)) + " fertig", status_t::good);
				}
				it = this->foundations.erase(it);
				continue;
			}
			++it;
		}
		population = count_population(state, now);
	}

	// --- snapshot ---
	Snapshot snap;
	snap.player = player_id;
	snap.time = t;
	if (state->has_player(player_id)) {
		snap.resources = state->get_player(player_id)->get_resources().get();
	}
	auto pop = population[player_id];
	snap.population = pop.used();
	snap.population_cap = pop.cap();
	snap.selection_count = own.size();
	if (not own.empty()) {
		snap.selection_id = entity_name(own.front());
		snap.selection_label = label_of(snap.selection_id);
		auto constructable = component_of<ConstructableComp>(own.front(), component::component_t::CONSTRUCTABLE);
		if (constructable != nullptr and not constructable->is_complete()) {
			snap.construction = constructable->get_progress();
		}
	}

	// options: buildings of the first villager, else the units of the first building
	std::shared_ptr<BuilderComp> builder;
	std::shared_ptr<GameEntity> producer;
	for (const auto &entity : own) {
		if (builder == nullptr) {
			builder = component_of<BuilderComp>(entity, component::component_t::BUILDER);
		}
		if (producer == nullptr and entity->has_component(component::component_t::PRODUCTION_QUEUE)
		    and is_complete(entity)) {
			producer = entity;
		}
	}
	auto availability = [&](Option &option, size_t queue_size) {
		auto missing = missing_resource(snap.resources, option.cost);
		if (missing) {
			option.available = false;
			option.reason = missing_message(*missing);
		}
		else if (not option.building and queue_size >= MAX_QUEUE) {
			option.available = false;
			option.reason = "Warteschlange voll";
		}
	};
	if (builder != nullptr) {
		for (const auto &b : builder->get_buildables()) {
			Option option;
			const auto *entry = known(b.name);
			option.code = entry ? entry->code : 0;
			option.id = b.name;
			option.label = label_of(b.name);
			option.icon = entry ? entry->icon : "hammer";
			option.building = true;
			option.cost = b.cost;
			option.time = b.time;
			availability(option, 0);
			snap.options.push_back(option);
		}
	}
	else if (producer != nullptr) {
		auto production = component_of<ProductionComp>(producer, component::component_t::PRODUCTION_QUEUE);
		for (const auto &c : production->get_creatables()) {
			Option option;
			const auto *entry = known(c.name);
			option.code = entry ? entry->code : 0;
			option.id = c.name;
			option.label = label_of(c.name);
			option.icon = entry ? entry->icon : "sword";
			option.building = false;
			option.cost = c.cost;
			option.time = c.time;
			availability(option, production->get_queue().size());
			snap.options.push_back(option);
		}
		QueueState queue;
		queue.building = producer->get_id();
		queue.label = label_of_entity(producer);
		for (const auto &item : production->get_queue().get_items()) {
			const auto *entry = known(item.name);
			queue.items.push_back({item.name, label_of(item.name), entry ? entry->icon : "sword"});
		}
		queue.progress = production->get_queue().progress(t);
		queue.waiting_for_housing = production->get_queue().is_waiting();
		if (production->rally_point) {
			const auto &r = *production->rally_point;
			queue.rally = std::array<double, 3>{r.ne.to_double(), r.se.to_double(), r.up.to_double()};
			using rally_t = ProductionComp::rally_t;
			const auto kind = production->rally_target ? production->rally_target->kind : rally_t::GROUND;
			queue.rally_kind = kind == rally_t::RESOURCE ? "resource" : kind == rally_t::ENTITY ? "entity"
			                 : kind == rally_t::ENEMY    ? "enemy"
			                                             : "ground";
		}
		snap.queue = queue;
	}

	// orders (XR fork): every training queue and foundation of the player
	for (const auto &[id, entity] : entities) {
		if (not entity->has_component(component::component_t::OWNERSHIP) or owner_of(entity, now) != player_id
		    or (state->get_combat() != nullptr and state->get_combat()->is_dead(id))) {
			continue;
		}
		auto production = component_of<ProductionComp>(entity, component::component_t::PRODUCTION_QUEUE);
		auto constructable = component_of<ConstructableComp>(entity, component::component_t::CONSTRUCTABLE);
		if ((production == nullptr or production->get_queue().size() == 0 or not is_complete(entity))
		    and (constructable == nullptr or constructable->is_complete())) {
			continue;
		}
		auto pos = position_of(entity, now);
		if (production != nullptr and production->get_queue().size() > 0 and is_complete(entity)) {
			const auto &head = production->get_queue().get_items().front();
			const auto *entry = known(head.name);
			Order order;
			order.building = id;
			order.building_label = label_of_entity(entity);
			order.label = label_of(head.name);
			order.icon = entry ? entry->icon : "sword";
			order.construction = false;
			order.count = production->get_queue().size();
			order.remaining = production->get_queue().remaining(t);
			order.progress = production->get_queue().progress(t);
			order.ne = pos.ne.to_double();
			order.se = pos.se.to_double();
			snap.orders.push_back(order);
		}
		if (constructable != nullptr and not constructable->is_complete()) {
			const auto name = entity_name(entity);
			const auto *entry = known(name);
			Order order;
			order.building = id;
			order.building_label = label_of(name);
			order.label = "Bau";
			order.icon = entry ? entry->icon : "hammer";
			order.construction = true;
			order.count = 1;
			order.progress = constructable->get_progress();
			// one builder: the rest of the build time
			order.remaining = std::max(0.0, (1.0 - order.progress) * constructable->get_build_time());
			order.ne = pos.ne.to_double();
			order.se = pos.se.to_double();
			snap.orders.push_back(order);
		}
	}
	std::stable_sort(snap.orders.begin(), snap.orders.end(), [](const Order &a, const Order &b) {
		if (a.construction != b.construction) {
			return not a.construction;
		}
		if ((a.remaining < 0.0) != (b.remaining < 0.0)) {
			return a.remaining >= 0.0;
		}
		return a.remaining < b.remaining;
	});

	std::lock_guard<std::mutex> lock{this->mutex};
	// the placement mode ends if the selection cannot build the building any more
	if (not this->placement.empty()) {
		bool can = false;
		for (const auto &option : snap.options) {
			can = can or (option.building and option.id == this->placement);
		}
		if (not can) {
			log::log(INFO << "Production: placement of " << this->placement << " ends (selection changed)");
			this->placement.clear();
		}
	}
	snap.status = this->current.status;
	snap.status_kind = this->current.status_kind;
	snap.status_seq = this->current.status_seq;
	snap.status_time = this->current.status_time;
	this->current = std::move(snap);
}


// ---- placement preview (XR fork) -----------------------------------------------------------

void Production::set_placement_cursor(const coord::phys3 &ground_hit) {
	std::lock_guard<std::mutex> lock{this->mutex};
	if (this->preview_cursor and this->preview_cursor->ne == ground_hit.ne and this->preview_cursor->se == ground_hit.se) {
		return;
	}
	this->preview_cursor = ground_hit;
	this->preview_dirty = true;
}

PlacementPreview Production::placement_preview() const {
	std::lock_guard<std::mutex> lock{this->mutex};
	return this->preview;
}

void Production::update_placement_preview(const std::shared_ptr<GameState> &state, const time::time_t &now) {
	std::string id;
	std::optional<coord::phys3> cursor;
	std::vector<entity_id_t> selected;
	player_id_t player_id;
	bool dirty;
	{
		std::lock_guard<std::mutex> lock{this->mutex};
		id = this->placement;
		cursor = this->preview_cursor;
		selected = this->selection;
		player_id = this->player;
		dirty = this->preview_dirty;
		this->preview_dirty = false;
		if (id.empty() or not cursor) {
			this->preview = PlacementPreview{};
			if (id.empty()) {
				this->preview_cursor.reset();
			}
			return;
		}
	}
	// the cursor moved, or units may have walked onto the footprint (4 Hz)
	const double t = now.to_double();
	if (not dirty and t >= this->preview_time and t < this->preview_time + 0.25) {
		return;
	}
	this->preview_time = t;
	if (state == nullptr or state->get_map() == nullptr) {
		return;
	}

	// a selected own villager that can build it (same choice as place_at)
	std::shared_ptr<GameEntity> builder_entity;
	const BuilderComp::Buildable *buildable = nullptr;
	for (auto sel_id : selected) {
		auto entity = find_entity(state, sel_id);
		if (entity == nullptr or not entity->has_component(component::component_t::OWNERSHIP)
		    or owner_of(entity, now) != player_id) {
			continue;
		}
		auto builder = component_of<BuilderComp>(entity, component::component_t::BUILDER);
		if (builder == nullptr) {
			continue;
		}
		if (const auto *b = builder->find(id)) {
			builder_entity = entity;
			buildable = b;
			break;
		}
	}
	if (buildable == nullptr) {
		std::lock_guard<std::mutex> lock{this->mutex};
		this->preview = PlacementPreview{};
		return;
	}

	PlacementPreview result;
	result.active = true;
	result.id = id;
	result.radius = buildable->radius;

	// idle animation of the building (cached per building)
	auto anim = this->preview_animations.find(buildable->fqon);
	if (anim == this->preview_animations.end()) {
		std::string path;
		try {
			const auto &view = state->get_db_view();
			auto obj = view->get_object(buildable->fqon);
			for (const auto &ability_val : obj.get_set("GameEntity.abilities")) {
				auto ability = view->get_object(object_name(ability_val));
				if (ability.get_parents()[0] == "engine.ability.type.Idle") {
					path = animation_of(ability);
					break;
				}
			}
		}
		catch (std::exception &e) {
			log::log(WARN << "Production: no idle animation for the preview of " << buildable->fqon << ": " << e.what());
		}
		anim = this->preview_animations.emplace(buildable->fqon, path).first;
	}
	result.animation = anim->second;

	// same footprint and tile rules as place_at()
	auto map = state->get_map();
	auto hit = map->pick_terrain(*cursor);
	auto [ane, ase] = snap_anchor(hit.ne.to_double(), hit.se.to_double(), buildable->radius);
	auto tiles = building_tiles(ane, ase, buildable->radius);
	std::set<std::pair<long, long>> unit_tiles;
	for (const auto &[eid, entity] : state->get_game_entities()) {
		if (is_unit(entity)) {
			auto p = position_of(entity, now);
			unit_tiles.insert({static_cast<long>(std::floor(p.ne.to_double())),
			                   static_cast<long>(std::floor(p.se.to_double()))});
		}
	}
	auto land = grid_of(builder_entity, state);
	auto water = grid_of(builder_entity, state, "Water");
	const auto size = map->get_size();
	auto check = check_placement(tiles, [&](const econ::tile_pos &tp) {
		if (tp.ne < 0 or tp.se < 0 or tp.ne >= static_cast<long>(size[0]) or tp.se >= static_cast<long>(size[1])) {
			return tile_state_t::OUTSIDE;
		}
		coord::tile tile{tp.ne, tp.se};
		if (land and not map->is_passable(*land, tile)) {
			if (water and map->is_passable(*water, tile)) {
				return tile_state_t::WATER;
			}
			return tile_state_t::BLOCKED;
		}
		if (not terrain_buildable(map->terrain_name(tile))) {
			return tile_state_t::WATER;
		}
		if (unit_tiles.contains({tp.ne, tp.se})) {
			return tile_state_t::OCCUPIED;
		}
		return tile_state_t::FREE;
	});
	coord::phys3 anchor{coord::phys_t{ane}, coord::phys_t{ase}, coord::phys_t{0.0}};
	auto on_ground = map->on_terrain(anchor);
	result.anchor_ne = ane;
	result.anchor_se = ase;
	result.anchor_up = on_ground.up.to_double();
	result.valid = check == placement_t::OK;
	result.reason = result.valid ? std::string{} : std::string{placement_message(check)};

	std::lock_guard<std::mutex> lock{this->mutex};
	if (this->placement == id) {
		this->preview = std::move(result);
	}
}

} // namespace openage::gamestate::prod
