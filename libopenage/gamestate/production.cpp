// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "production.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "log/log.h"
#include "log/message.h"

#include "assets/mod_manager.h"

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
#include "gamestate/component/api_component.h"
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
#include "gamestate/food.h"
#include "gamestate/food_math.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/manager.h"
#include "gamestate/map.h"
#include "gamestate/player.h"
#include "gamestate/production_math.h"
#include "gamestate/tech.h"
#include "gamestate/tech_math.h"
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
	if (not entity->has_component(component::component_t::MOVE)) {
		return false;
	}
	// XR fork W3: herdables and game are no population (and do not block foundations)
	auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
	return h == nullptr or not food::is_animal(h->kind);
}

/// complete building (or one without construction)
bool is_complete(const std::shared_ptr<GameEntity> &entity) {
	auto constructable = component_of<ConstructableComp>(entity, component::component_t::CONSTRUCTABLE);
	return constructable == nullptr or constructable->is_complete();
}

// ---- XR fork W3: farms, docks, fishing traps

/// buildings that stand on their tiles without blocking them (farmers walk on farms, ships over traps)
bool is_field_name(const std::string &name) {
	return name == "Farm" or name == "FishingTrap";
}

/**
 * Placement terrain of a building on water (docs/aoe2-spec 04 §4.2 rule 6): the placement
 * and side terrains of its Genie data (GenieUnitData.placement_terrain0/1,
 * placement_side_terrain0/1, docs/KONVERTER-FELDER.md) as terrain objects of
 * GenieData.terrains; the fishing trap has none and stands on any open water.
 */
struct WaterRule {
	/// the building stands on water
	bool water = false;
	/// terrain objects allowed under the footprint (empty: any open water)
	std::set<std::string> place;
	/// terrain objects of which at least one must be next to the footprint (empty: none needed)
	std::set<std::string> side;
	/// terrain objects the terrain restriction of the building allows (factor > 0.05, spec 04 §4.2
	/// rule 2; empty: the water terrains)
	std::set<std::string> allowed;
};

bool water_terrain_name(const std::string &fqon) {
	auto ends_with = [&](const char *suffix) {
		const std::string s{suffix};
		return fqon.size() >= s.size() and fqon.compare(fqon.size() - s.size(), s.size(), s) == 0;
	};
	return ends_with(".Water") or ends_with(".Water2") or ends_with(".Water3");
}

WaterRule water_rule(const std::shared_ptr<nyan::View> &view, const nyan::fqon_t &fqon) {
	static std::mutex mutex;
	static std::unordered_map<std::string, WaterRule> cache;
	std::lock_guard<std::mutex> lock{mutex};
	auto it = cache.find(fqon);
	if (it != cache.end()) {
		return it->second;
	}
	WaterRule rule;
	try {
		auto obj = view->get_object(fqon);
		int64_t place[2] = {-1, -1};
		int64_t side[2] = {-1, -1};
		int64_t restriction = -1;
		for (const auto &ability_val : obj.get_set("GameEntity.abilities")) {
			auto ability = view->get_object(object_name(ability_val));
			if (ability.get_parents()[0] != "engine.xr.ability.GenieUnit") {
				continue;
			}
			place[0] = ability.get_int("GenieUnitData.placement_terrain0");
			place[1] = ability.get_int("GenieUnitData.placement_terrain1");
			side[0] = ability.get_int("GenieUnitData.placement_side_terrain0");
			side[1] = ability.get_int("GenieUnitData.placement_side_terrain1");
			restriction = ability.get_int("GenieUnitData.terrain_restriction");
		}
		rule.water = place[0] >= 0 or place[1] >= 0 or short_name(fqon) == "FishingTrap";
		if (rule.water) {
			const auto modpack = fqon.substr(0, fqon.find('.'));
			auto data = view->get_object(modpack + ".data.util.xr.genie.GenieData");
			std::unordered_map<int64_t, std::string> terrains;
			for (const auto &[key, value] : data.get_dict("GenieData.terrains")) {
				auto id = std::dynamic_pointer_cast<nyan::Int>(key.get_ptr());
				if (id == nullptr) {
					continue;
				}
				const auto n = id->get();
				terrains[n] = object_name(value);
				if (n == place[0] or n == place[1]) {
					rule.place.insert(object_name(value));
				}
				if (n == side[0] or n == side[1]) {
					rule.side.insert(object_name(value));
				}
			}
			// terrain restriction of the building: where it may stand at all
			for (const auto &r_val : data.get_orderedset("GenieData.terrain_restrictions")) {
				auto r = view->get_object(object_name(r_val));
				if (r.get_int("GenieTerrainRestriction.restriction_id") != restriction) {
					continue;
				}
				for (const auto &[key, value] : r.get_dict("GenieTerrainRestriction.factors")) {
					auto id = std::dynamic_pointer_cast<nyan::Int>(key.get_ptr());
					auto factor = std::dynamic_pointer_cast<nyan::Float>(value.get_ptr());
					if (id != nullptr and factor != nullptr and factor->get() > 0.05 and terrains.contains(id->get())) {
						rule.allowed.insert(terrains[id->get()]);
					}
				}
			}
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Production: placement terrain of " << fqon << " not readable (" << err.what()
		              << "), placed like a land building");
	}
	if (rule.water) {
		log::log(INFO << "Production: " << short_name(fqon) << " stands on water (" << rule.place.size()
		              << " placement terrains, " << rule.side.size() << " shore terrains, " << rule.allowed.size()
		              << " terrains of its restriction)");
	}
	cache.emplace(fqon, rule);
	return rule;
}

/// result of a placement check (same rules for the foundation and the ghost)
struct PlaceCheck {
	bool ok = false;
	std::string message;
	std::vector<econ::tile_pos> tiles;
	double ne = 0.0;
	double se = 0.0;
};

/**
 * Check the placement of a building at a clicked ground point: land buildings on free,
 * buildable land without units (and not on farms, XR fork W3), water buildings by their
 * water rule (docks next to the shore, traps on open water).
 */
PlaceCheck check_place(const std::shared_ptr<GameState> &state,
                       const std::shared_ptr<GameEntity> &builder,
                       const BuilderComp::Buildable &buildable,
                       const coord::phys3 &ground_hit,
                       const time::time_t &now) {
	PlaceCheck result;
	auto map = state->get_map();
	auto hit = map->pick_terrain(ground_hit);
	auto [ane, ase] = snap_anchor(hit.ne.to_double(), hit.se.to_double(), buildable.radius);
	result.ne = ane;
	result.se = ase;
	result.tiles = building_tiles(ane, ase, buildable.radius);

	// tiles where units stand
	std::set<std::pair<long, long>> unit_tiles;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (is_unit(entity)) {
			auto p = position_of(entity, now);
			unit_tiles.insert({static_cast<long>(std::floor(p.ne.to_double())),
			                   static_cast<long>(std::floor(p.se.to_double()))});
		}
	}
	auto land = grid_of(builder, state, "Land");
	auto water = grid_of(builder, state, "Water");
	auto food_state = state->get_food();
	const auto size = map->get_size();
	auto outside = [&](const econ::tile_pos &t) {
		return t.ne < 0 or t.se < 0 or t.ne >= static_cast<long>(size[0]) or t.se >= static_cast<long>(size[1]);
	};
	auto field_there = [&](const econ::tile_pos &t) {
		return food_state != nullptr and food_state->field_tile(t.ne, t.se);
	};

	const auto rule = water_rule(state->get_db_view(), buildable.fqon);
	if (rule.water) {
		const bool need_place = not rule.place.empty();
		auto res = food::check_water_placement(result.tiles, not rule.side.empty(), need_place, [&](const econ::tile_pos &t) {
			if (outside(t)) {
				return food::water_tile_t::OUTSIDE;
			}
			coord::tile tile{t.ne, t.se};
			const auto name = map->terrain_name(tile);
			if (rule.side.contains(name)) {
				return food::water_tile_t::SHORE;
			}
			// placement terrain of the data (dock: water, shallows); other water its restriction allows
			// (open water: not walkable for land units)
			const bool placement = need_place and rule.place.contains(name);
			const bool open_water = water_terrain_name(name)
			                        and (rule.allowed.empty() or rule.allowed.contains(name));
			if (not placement and not open_water) {
				return water_terrain_name(name) ? food::water_tile_t::OTHER_WATER : food::water_tile_t::LAND;
			}
			if ((water and not map->is_passable(*water, tile)) or field_there(t)) {
				return food::water_tile_t::BLOCKED;
			}
			if (unit_tiles.contains({t.ne, t.se})) {
				return food::water_tile_t::OCCUPIED;
			}
			return placement ? food::water_tile_t::WATER : food::water_tile_t::ALLOWED;
		});
		result.ok = res == food::water_place_t::OK;
		result.message = food::water_place_message(res);
		return result;
	}

	auto check = check_placement(result.tiles, [&](const econ::tile_pos &t) {
		if (outside(t)) {
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
		// XR fork W3: farms do not block their tiles, but nothing else is built on them
		if (field_there(t)) {
			return tile_state_t::BLOCKED;
		}
		if (unit_tiles.contains({t.ne, t.se})) {
			return tile_state_t::OCCUPIED;
		}
		return tile_state_t::FREE;
	});
	result.ok = check == placement_t::OK;
	result.message = placement_message(check);
	return result;
}

/// XR fork W3: the player has a finished mill (farms are offered only then, docs/aoe2-spec 13)
bool has_mill(const tech::Facts &facts) {
	for (const auto &b : facts.built) {
		if (short_name(b) == "Mill") {
			return true;
		}
	}
	return false;
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

/// nyan game entity of an entity ("...town_center.TownCenter"), from the names of its abilities
std::string fqon_of_entity(const std::shared_ptr<GameEntity> &entity) {
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
			return ability.substr(0, dot);
		}
	}
	return {};
}

/// technologies queued in the buildings of every player (XR fork W1: researched once)
std::unordered_map<player_id_t, std::set<std::string>> queued_research_all(const std::shared_ptr<GameState> &state,
                                                                           const time::time_t &time) {
	std::unordered_map<player_id_t, std::set<std::string>> out;
	const auto &combat = state->get_combat();
	for (const auto &[id, entity] : state->get_game_entities()) {
		auto production = component_of<ProductionComp>(entity, component::component_t::PRODUCTION_QUEUE);
		if (production == nullptr or production->get_queue().size() == 0
		    or (combat != nullptr and combat->is_dead(id))) {
			continue;
		}
		for (const auto &item : production->get_queue().get_items()) {
			if (item.research) {
				out[owner_of(entity, time)].insert(item.fqon);
			}
		}
	}
	return out;
}

/// "Spieler 2 (Franken)" for messages about other players
std::string player_text(const std::shared_ptr<tech::TechState> &techs, player_id_t player) {
	std::string out = "Spieler " + std::to_string(player + 1);
	if (techs) {
		if (const auto *s = techs->find(player); s != nullptr and not s->civ.empty()) {
			out += " (" + civ_label(s->civ) + ")";
		}
	}
	return out;
}

} // namespace


std::string sprite_path(const std::string &path) {
	return fix_shared_path(path);
}

void init_components(const std::shared_ptr<openage::event::EventLoop> &loop,
                     const std::shared_ptr<nyan::View> &owner_db_view,
                     const std::shared_ptr<GameEntity> &entity,
                     const nyan::fqon_t &nyan_entity) {
	auto nyan_obj = owner_db_view->get_object(nyan_entity);
	nyan::set_t abilities = nyan_obj.get_set("GameEntity.abilities");

	std::optional<nyan::Object> create_ability;
	std::optional<nyan::Object> research_ability;
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
					if (not has_mechanic(name)) {
						// XR fork W1: hidden until the engine has its mechanic (catalog.h)
						continue;
					}
					auto cost = cost_of(owner_db_view, creatable.get_object("CreatableGameEntity.cost"));
					tech::Condition condition;
					try {
						condition = tech::read_condition_set(owner_db_view,
						                                     creatable.get_set("CreatableGameEntity.condition"));
						// XR fork W4: the watch tower waits for its own completion in the data
						condition = tech::fix_self_condition(owner_db_view, fqon, condition);
					}
					catch (std::exception &err) {
						log::log(WARN << "Production: condition of " << name << " not readable: " << err.what());
					}
					if (is_placed(owner_db_view, creatable)) {
						BuilderComp::Buildable buildable;
						buildable.name = name;
						buildable.fqon = fqon;
						buildable.cost = cost;
						buildable.radius = hitbox_radius(owner_db_view, fqon, 1.0);
						buildable.condition = condition;
						buildings.push_back(buildable);
					}
					else {
						ProductionComp::Creatable unit;
						unit.name = name;
						unit.fqon = fqon;
						unit.cost = cost;
						auto time = creatable.get_float("CreatableGameEntity.creation_time");
						unit.time = time > 0.0 ? time : DEFAULT_TRAIN_TIME;
						unit.condition = condition;
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
			else if (ability_parent == "engine.ability.type.Research") {
				// XR fork W1: what it researches is read per player (tech.h)
				research_ability = ability_obj;
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
	else if (research_ability) {
		// XR fork W1: buildings that only research (blacksmith, university, mill, camps)
		entity->add_component(std::make_shared<ProductionComp>(loop, *research_ability,
		                                                       std::vector<ProductionComp::Creatable>{}));
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
	if (code == CMD_RESEED_FARM) {
		// XR fork W3: mill "Feld neu einsäen"
		this->reseed();
		return;
	}
	const auto *entry = known(code);
	if (entry == nullptr) {
		this->set_status("Unbekannter Befehl", status_t::warn);
		return;
	}
	if (entry->kind == entry_t::TECH) {
		this->research(entry->name);
	}
	else if (entry->building) {
		this->start_placement(entry->name);
	}
	else {
		this->train(entry->name);
	}
}

void Production::reseed() {
	Request request;
	request.kind = Request::kind_t::RESEED;
	this->push(std::move(request));
}

void Production::reseed_for(player_id_t player) {
	Request request;
	request.kind = Request::kind_t::RESEED;
	request.for_player = player;
	this->push(std::move(request));
}

void Production::grant_resources(player_id_t player, double amount) {
	Request request;
	request.kind = Request::kind_t::TEST_RESOURCES;
	request.for_player = player;
	request.pos = coord::phys3{coord::phys_t{amount}, coord::phys_t{0.0}, coord::phys_t{0.0}};
	this->push(std::move(request));
}

void Production::test_spawn(player_id_t player, const std::string &name, double ne, double se) {
	Request request;
	request.kind = Request::kind_t::TEST_SPAWN;
	request.for_player = player;
	request.id = name;
	request.pos = coord::phys3{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}};
	this->push(std::move(request));
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

void Production::research(const std::string &id) {
	Request request;
	request.kind = Request::kind_t::RESEARCH;
	request.id = id;
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

void Production::grant_research(player_id_t player, const std::string &id) {
	Request request;
	request.kind = Request::kind_t::GRANT_RESEARCH;
	request.id = id;
	request.for_player = player;
	this->push(std::move(request));
}

void Production::research_for(player_id_t player, entity_id_t building, const std::string &id) {
	Request request;
	request.kind = Request::kind_t::RESEARCH;
	request.id = id;
	request.building = building;
	request.for_player = player;
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

std::vector<std::string> available_research(const std::shared_ptr<GameState> &state,
                                            player_id_t player,
                                            const std::shared_ptr<GameEntity> &building,
                                            const time::time_t &time) {
	std::vector<std::string> out;
	auto techs = state->get_tech();
	if (techs == nullptr or building == nullptr or not is_complete(building)
	    or not building->has_component(component::component_t::PRODUCTION_QUEUE)
	    or owner_of(building, time) != player) {
		return out;
	}
	auto queued = queued_research_all(state, time)[player];
	const auto *s = techs->find(player);
	const tech::Facts facts = s ? s->facts : tech::Facts{};
	const int age = s ? s->age : tech::DARK_AGE;
	for (const auto &r : techs->researchables(state, player, fqon_of_entity(building))) {
		auto offer = tech::offer_state(r.condition, facts, age, facts.researched.contains(r.fqon),
		                               queued.contains(r.fqon), has_mechanic(r.name, true));
		if (offer == tech::offer_t::AVAILABLE) {
			out.push_back(r.name);
		}
	}
	return out;
}

bool research_queued(const std::shared_ptr<GameState> &state, player_id_t player, const time::time_t &time) {
	auto all = queued_research_all(state, time);
	auto it = all.find(player);
	return it != all.end() and not it->second.empty();
}

bool set_rally_point(const std::shared_ptr<GameState> &state,
                     const std::shared_ptr<GameEntity> &building,
                     const coord::phys3 &ground,
                     const std::shared_ptr<GameEntity> &target_entity,
                     const time::time_t &time) {
	auto production = component_of<ProductionComp>(building, component::component_t::PRODUCTION_QUEUE);
	if (production == nullptr or production->get_creatables().empty()) {
		// XR fork W1: buildings that only research have no rally point
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

	// XR fork W1: technologies, ages, civilisations of the players
	auto techs = state->get_tech();
	if (tick and techs) {
		// completed buildings count for the unlock conditions (once built stays built)
		for (const auto &[id, entity] : entities) {
			auto constructable = component_of<ConstructableComp>(entity, component::component_t::CONSTRUCTABLE);
			if (constructable != nullptr and constructable->is_complete()
			    and entity->has_component(component::component_t::OWNERSHIP)
			    and not(state->get_combat() != nullptr and state->get_combat()->is_dead(id))) {
				techs->building_done(owner_of(entity, now), fqon_of_entity(entity));
			}
		}
	}
	auto facts_of = [&](player_id_t owner) {
		const tech::PlayerTech *s = techs ? techs->find(owner) : nullptr;
		return s ? s->facts : tech::Facts{};
	};
	auto age_of_player = [&](player_id_t owner) {
		const tech::PlayerTech *s = techs ? techs->find(owner) : nullptr;
		return s ? s->age : tech::DARK_AGE;
	};
	auto unit_label = [&](player_id_t owner, const std::string &fqon) {
		return techs ? techs->unit_label(owner, fqon) : label_of(short_name(fqon));
	};
	auto queued_research = queued_research_all(state, now);

	// XR fork W3: costs of a farm for a player (build list of its villagers; a reseed costs the same)
	auto farm_cost = [&](player_id_t owner) -> std::optional<resource_amounts_t> {
		for (const auto &[id, entity] : entities) {
			auto b = component_of<BuilderComp>(entity, component::component_t::BUILDER);
			if (b == nullptr or not entity->has_component(component::component_t::OWNERSHIP)
			    or owner_of(entity, now) != owner) {
				continue;
			}
			if (const auto *farm = b->find("Farm")) {
				return farm->cost;
			}
		}
		return std::nullopt;
	};
	// XR fork W3: a finished mill of the player (reseeds are queued there)
	auto has_own_mill = [&](player_id_t owner) {
		for (const auto &[id, entity] : entities) {
			if (entity->has_component(component::component_t::OWNERSHIP) and owner_of(entity, now) == owner
			    and entity_name(entity) == "Mill" and is_complete(entity)
			    and not(state->get_combat() != nullptr and state->get_combat()->is_dead(id))) {
				return true;
			}
		}
		return false;
	};

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
			std::string locked_reason;
			for (const auto &entity : candidates) {
				auto production = component_of<ProductionComp>(entity, component::component_t::PRODUCTION_QUEUE);
				if (production == nullptr or not is_complete(entity)) {
					continue;
				}
				if (production->get_creatables().empty()) {
					continue;
				}
				const auto *c = request.id.empty() ? &production->get_creatables().front()
				                                   : production->find(request.id);
				if (c == nullptr) {
					continue;
				}
				// XR fork W1: unlock condition of the data (age, buildings, technologies)
				const auto facts = facts_of(owner_of(entity, now));
				if (not tech::evaluate(c->condition, facts)) {
					locked_reason = tech::missing_text(c->condition, facts);
					continue;
				}
				if (production->get_queue().size() < best_size) {
					best = entity;
					creatable = c;
					best_size = production->get_queue().size();
				}
			}
			if (best == nullptr) {
				status(not locked_reason.empty() ? locked_reason
				       : request.id.empty()     ? std::string{"Die Auswahl kann nichts ausbilden"}
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
			status(unit_label(owner, creatable->fqon) + " wird ausgebildet", status_t::info);
		} break;

		case Request::kind_t::RESEARCH: {
			// XR fork W1: a technology in the first own building that offers it now
			std::vector<std::shared_ptr<GameEntity>> candidates;
			if (request.for_player or request.building != 0) {
				auto building = find_entity(state, request.building);
				if (building != nullptr and building->has_component(component::component_t::OWNERSHIP)
				    and (not request.for_player or owner_of(building, now) == *request.for_player)) {
					candidates.push_back(building);
				}
			}
			else {
				candidates = own;
			}
			std::shared_ptr<GameEntity> best;
			std::optional<tech::Researchable> what;
			std::string reason;
			for (const auto &entity : candidates) {
				auto production = component_of<ProductionComp>(entity, component::component_t::PRODUCTION_QUEUE);
				if (production == nullptr or not is_complete(entity) or techs == nullptr) {
					continue;
				}
				const auto owner = owner_of(entity, now);
				for (const auto &r : techs->researchables(state, owner, fqon_of_entity(entity))) {
					if (r.name != request.id) {
						continue;
					}
					const auto facts = facts_of(owner);
					const bool researched = facts.researched.contains(r.fqon);
					const bool queued = queued_research[owner].contains(r.fqon);
					if (researched) {
						reason = label_of(r.name, true) + " ist schon erforscht";
					}
					else if (queued) {
						reason = label_of(r.name, true) + " wird schon erforscht";
					}
					else if (not has_mechanic(r.name, true)) {
						reason = label_of(r.name, true) + ": " + known(r.name, true)->missing;
					}
					else if (not tech::evaluate(r.condition, facts)) {
						reason = label_of(r.name, true) + ": " + tech::missing_text(r.condition, facts);
					}
					else if (production->get_queue().size() >= MAX_QUEUE) {
						reason = "Warteschlange voll";
					}
					else {
						best = entity;
						what = r;
					}
					break;
				}
				if (best) {
					break;
				}
			}
			if (best == nullptr or not what) {
				status(reason.empty() ? "Die Auswahl kann " + label_of(request.id, true) + " nicht erforschen" : reason,
				       status_t::warn);
				break;
			}
			auto production = component_of<ProductionComp>(best, component::component_t::PRODUCTION_QUEUE);
			auto owner = owner_of(best, now);
			auto &stock = state->get_player(owner)->get_resources();
			if (not stock.spend(what->cost)) {
				auto missing = missing_resource(stock.get(), what->cost);
				status(missing ? missing_message(*missing) : "Nicht genug Rohstoffe", status_t::warn);
				log::log(INFO << "Production: player " << owner << " cannot afford research " << what->name);
				break;
			}
			TrainQueue::Item item;
			item.name = what->name;
			item.fqon = what->fqon;
			item.cost = what->cost;
			item.time = what->time > 0.0 ? what->time : DEFAULT_TRAIN_TIME;
			item.research = true;
			production->get_queue().push(item);
			queued_research[owner].insert(what->fqon);
			log::log(INFO << "Production: player " << owner << " queues research " << what->name << " in "
			              << entity_name(best) << " (entity " << best->get_id() << ", queue "
			              << production->get_queue().size() << "/" << MAX_QUEUE << ", " << item.time << " s), "
			              << cost_str(what->cost));
			status(label_of(what->name, true) + " wird erforscht", status_t::info);
		} break;

		case Request::kind_t::GRANT_RESEARCH: {
			// XR fork W1, test option: the technology as researched, found in any building of the player
			const auto player = request.for_player.value_or(player_id);
			std::optional<std::string> fqon;
			for (const auto &[id, entity] : entities) {
				if (not techs or not entity->has_component(component::component_t::PRODUCTION_QUEUE)
				    or not entity->has_component(component::component_t::OWNERSHIP) or owner_of(entity, now) != player) {
					continue;
				}
				for (const auto &r : techs->researchables(state, player, fqon_of_entity(entity))) {
					if (r.name == request.id) {
						fqon = r.fqon;
					}
				}
				if (fqon) {
					break;
				}
			}
			if (not fqon) {
				log::log(WARN << "Production: test grant of " << request.id << " for player " << player
				              << ": no building offers it");
				break;
			}
			log::log(INFO << "Production: test grant of research " << request.id << " for player " << player);
			techs->research_done(state, loop, player, *fqon, now);
		} break;

		case Request::kind_t::RESEED: {
			// XR fork W3: mill "Feld neu einsäen" (docs/aoe2-spec 13 L14): costs of a farm now
			const auto owner = request.for_player.value_or(player_id);
			auto food_state = state->get_food();
			if (food_state == nullptr or not has_own_mill(owner)) {
				status("Feld neu einsäen: zuerst eine Mühle bauen", status_t::warn);
				break;
			}
			auto cost = farm_cost(owner);
			if (not cost) {
				status("Feld neu einsäen: keine Dorfbewohner, die Felder bauen", status_t::warn);
				break;
			}
			auto reason = food_state->queue_reseed(state, owner, *cost);
			if (not reason.empty()) {
				status(reason, status_t::warn);
				break;
			}
			status("Feld eingereiht (" + std::to_string(food_state->reseeds(owner)) + " in der Mühle)", status_t::info);
		} break;

		case Request::kind_t::TEST_RESOURCES: {
			// XR fork W3, test option of the replays
			const auto owner = request.for_player.value_or(player_id);
			if (state->has_player(owner)) {
				auto &stock = state->get_player(owner)->get_resources();
				for (size_t i = 0; i < RESOURCE_COUNT; ++i) {
					stock.add(static_cast<resource_t>(i), request.pos.ne.to_double());
				}
				log::log(INFO << "Production: test grant of " << request.pos.ne.to_double() << " of every resource for player "
				              << owner);
			}
		} break;

		case Request::kind_t::TEST_SPAWN: {
			// XR fork W3, test option of the replays: a finished entity at a tile
			const auto owner = request.for_player.value_or(player_id);
			std::string snake;
			for (char c : request.id) {
				if (std::isupper(static_cast<unsigned char>(c)) and not snake.empty()) {
					snake += '_';
				}
				snake += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			std::shared_ptr<GameEntity> spawned;
			for (const auto &modpack : state->get_mod_manager()->get_load_order()) {
				const auto fqon = modpack + ".data.game_entity.generic." + snake + "." + snake + "." + request.id;
				try {
					state->get_db_view()->get_object(fqon);
				}
				catch (std::exception &) {
					continue;
				}
				spawned = factory->add_game_entity(loop, state, owner, fqon);
				break;
			}
			if (spawned == nullptr) {
				log::log(WARN << "Production: test spawn of " << request.id << ": no such game entity");
				break;
			}
			auto map = state->get_map();
			auto pos = std::dynamic_pointer_cast<component::Position>(spawned->get_component(component::component_t::POSITION));
			pos->set_position(now, map->on_terrain(request.pos));
			pos->set_angle(now, coord::phys_angle_t::from_int(315));
			std::dynamic_pointer_cast<component::Ownership>(spawned->get_component(component::component_t::OWNERSHIP))
				->set_owner(now, owner);
			std::dynamic_pointer_cast<component::Activity>(spawned->get_component(component::component_t::ACTIVITY))->init(now);
			spawned->get_manager()->run_activity_system(now);
			state->add_game_entity(spawned);
			if (auto constructable = component_of<ConstructableComp>(spawned, component::component_t::CONSTRUCTABLE)) {
				if (not is_field_name(request.id)) {
					for (const auto &t : building_tiles(request.pos.ne.to_double(), request.pos.se.to_double(), constructable->radius)) {
						map->block_tile(coord::tile{t.ne, t.se}, now);
					}
				}
				prod::complete_building(spawned, now);
			}
			log::log(INFO << "Production: test spawn of " << request.id << " (entity " << spawned->get_id() << ") for player "
			              << owner << " at tile " << tile_str(request.pos));
		} break;

		case Request::kind_t::CANCEL:
		case Request::kind_t::CANCEL_AT: {
			bool done = false;
			for (const auto &entity : own) {
				auto production = component_of<ProductionComp>(entity, component::component_t::PRODUCTION_QUEUE);
				// XR fork W3: the reseeds follow the research items of a selected mill
				auto food_state = state->get_food();
				if (production != nullptr and food_state != nullptr and entity_name(entity) == "Mill"
				    and ((request.kind == Request::kind_t::CANCEL_AT and request.index >= production->get_queue().size())
				         or (request.kind == Request::kind_t::CANCEL and production->get_queue().size() == 0))) {
					const auto owner = owner_of(entity, now);
					const size_t reseeds = food_state->reseeds(owner);
					const size_t index = request.kind == Request::kind_t::CANCEL_AT
					                         ? request.index - production->get_queue().size()
					                         : (reseeds > 0 ? reseeds - 1 : 0);
					if (reseeds > 0 and food_state->cancel_reseed(state, owner, index)) {
						status("Neusäen abgebrochen, Kosten erstattet", status_t::info);
						done = true;
						break;
					}
				}
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
				status(label_of(item->name, item->research) + " abgebrochen", status_t::info);
				if (item->research) {
					queued_research[owner].erase(item->fqon);
				}
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
			// XR fork W1: unlock condition of the data (age, buildings)
			if (const auto facts = facts_of(owner_of(builders.front(), now));
			    not tech::evaluate(buildable->condition, facts)) {
				status(label_of(buildable->name) + ": " + tech::missing_text(buildable->condition, facts), status_t::warn);
				break;
			}
			// XR fork W3: farms only after a mill (docs/aoe2-spec 13: the button appears with the mill)
			if (buildable->name == "Farm" and not has_mill(facts_of(owner_of(builders.front(), now)))) {
				status("Farm: zuerst eine Mühle bauen", status_t::warn);
				break;
			}
			auto map = state->get_map();
			auto place = check_place(state, builders.front(), *buildable, request.pos, now);
			const auto &tiles = place.tiles;
			coord::phys3 anchor{coord::phys_t{place.ne}, coord::phys_t{place.se}, coord::phys_t{0.0}};
			if (not place.ok) {
				status(place.message, status_t::warn);
				log::log(INFO << "Production: " << request.id << " at tile " << tile_str(anchor)
				              << " rejected: " << place.message);
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
			if (is_field_name(buildable->name)) {
				// XR fork W3: farms and traps do not block their tiles; the ground shows the farm
				if (auto food_state = state->get_food()) {
					food_state->field_placed(state, building, now);
				}
			}
			else {
				for (const auto &t : tiles) {
					map->block_tile(coord::tile{t.ne, t.se}, now);
				}
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
				if (item.research) {
					// XR fork W1: the technology takes effect (patches, refresh, messages)
					log::log(INFO << "Production: research " << item.name << " done in " << entity_name(entity)
					              << " (entity " << entity->get_id() << ") at t=" << t << " s, queue left "
					              << production->get_queue().size());
					if (techs) {
						techs->research_done(state, loop, owner, item.fqon, now);
					}
					continue;
				}
				auto unit = spawn_unit(state, loop, factory, entity, *production, item.fqon, now);
				log::log(INFO << "Production: " << item.name << " (entity " << unit->get_id() << ") trained in "
				              << entity_name(entity) << " (entity " << entity->get_id() << ") at t=" << t
				              << " s, spawned at tile " << tile_str(position_of(unit, now)) << ", queue left "
				              << production->get_queue().size());
				if (owner == player_id) {
					this->set_status(unit_label(owner, item.fqon) + " bereit", status_t::good);
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
		// XR fork W1: researched technologies and ages (the age of every player is announced)
		if (techs) {
			for (const auto &m : techs->take_messages()) {
				if (m.age > 0) {
					this->set_status(tech::age_reached_text(player_text(techs, m.player), m.age),
					                 m.player == player_id ? status_t::good : status_t::info);
				}
				else if (m.player == player_id) {
					this->set_status(label_of(m.tech, true) + " erforscht", status_t::good);
				}
			}
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
		snap.selection_label = is_unit(own.front()) ? unit_label(player_id, fqon_of_entity(own.front()))
		                                           : label_of(snap.selection_id);
		auto constructable = component_of<ConstructableComp>(own.front(), component::component_t::CONSTRUCTABLE);
		if (constructable != nullptr and not constructable->is_complete()) {
			snap.construction = constructable->get_progress();
		}
	}
	// XR fork W3: food (or other resource) left in the first selected entity, also of gaia
	// (sheep, deer, carcass, farm, fish, trees, mines): line of the info panel
	if (not selected.empty()) {
		auto first = find_entity(state, selected.front());
		auto h = component_of<component::Harvestable>(first, component::component_t::HARVESTABLE);
		auto food_state = state->get_food();
		const bool decayed = food_state != nullptr and first != nullptr and food_state->is_decayed(first->get_id());
		if (h != nullptr and is_complete(first) and (not h->is_depleted() or decayed)) {
			if (h->kind == food::source_t::OTHER) {
				snap.selection_resource = std::string{resource_label(h->get_resource())} + " "
				                          + std::to_string(static_cast<long>(std::floor(h->get_amount())));
			}
			else {
				snap.selection_resource = food::amount_text(h->kind, h->get_amount(), h->dead, decayed);
			}
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
	// XR fork W1: unlock state of an entry for the HUD player (hidden: later age, no mechanic)
	const auto hud_facts = facts_of(player_id);
	const int hud_age = age_of_player(player_id);
	auto offer = [&](Option &option, const tech::Condition &condition, bool researched, bool queued, bool mechanic) {
		auto o = tech::offer_state(condition, hud_facts, hud_age, researched, queued, mechanic);
		if (o == tech::offer_t::LOCKED) {
			option.available = false;
			option.reason = queued ? std::string{"wird schon erforscht"} : tech::missing_text(condition, hud_facts);
		}
		return o != tech::offer_t::HIDDEN;
	};
	if (builder != nullptr) {
		for (const auto &b : builder->get_buildables()) {
			// XR fork W3: the farm button appears once a mill stands (docs/aoe2-spec 13)
			if (b.name == "Farm" and not has_mill(hud_facts)) {
				continue;
			}
			Option option;
			const auto *entry = known(b.name);
			option.code = entry ? entry->code : 0;
			option.id = b.name;
			option.label = label_of(b.name);
			option.icon = entry ? entry->icon : "hammer";
			option.building = true;
			option.cost = b.cost;
			option.time = b.time;
			if (not offer(option, b.condition, false, false, true)) {
				continue;
			}
			if (option.available) {
				availability(option, 0);
			}
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
			option.label = unit_label(player_id, c.fqon);
			option.icon = entry ? entry->icon : "sword";
			option.building = false;
			option.cost = c.cost;
			option.time = c.time;
			if (not offer(option, c.condition, false, false, true)) {
				continue;
			}
			if (option.available) {
				availability(option, production->get_queue().size());
			}
			snap.options.push_back(option);
		}
		// XR fork W1: technologies of the building (Research ability of the player's data)
		if (techs) {
			for (const auto &r : techs->researchables(state, player_id, fqon_of_entity(producer))) {
				Option option;
				const auto *entry = known(r.name, true);
				option.code = entry ? entry->code : 0;
				option.id = r.name;
				option.label = label_of(r.name, true);
				option.icon = entry ? entry->icon : "tech";
				option.building = false;
				option.research = true;
				option.effect = entry ? entry->effect : "";
				option.cost = r.cost;
				option.time = r.time;
				if (not offer(option, r.condition, hud_facts.researched.contains(r.fqon),
				              queued_research[player_id].contains(r.fqon), has_mechanic(r.name, true))) {
					continue;
				}
				if (option.available) {
					availability(option, production->get_queue().size());
				}
				snap.options.push_back(option);
			}
		}
		// XR fork W3: the mill re-sows farms (one command, docs/aoe2-spec 13 L14)
		auto food_state = state->get_food();
		const bool mill = food_state != nullptr and entity_name(producer) == "Mill";
		if (mill) {
			Option option;
			option.code = CMD_RESEED_FARM;
			option.id = "ReseedFarm";
			option.label = "Feld neu einsäen";
			option.icon = "farm";
			option.effect = "stellt ein Feld in die Warteschlange, das sich nach dem Verfall selbst erneuert";
			auto cost = farm_cost(player_id);
			if (cost) {
				option.cost = *cost;
				availability(option, food_state->reseeds(player_id));
			}
			else {
				option.available = false;
				option.reason = "Keine Dorfbewohner, die Felder bauen";
			}
			snap.options.push_back(option);
		}
		QueueState queue;
		queue.building = producer->get_id();
		queue.label = label_of_entity(producer);
		for (const auto &item : production->get_queue().get_items()) {
			const auto *entry = known(item.name, item.research);
			queue.items.push_back({item.name,
			                       item.research ? label_of(item.name, true) : unit_label(player_id, item.fqon),
			                       entry ? entry->icon : (item.research ? "tech" : "sword"),
			                       item.research});
		}
		// XR fork W3: queued reseeds as symbols after the research items (click = cancel, full refund)
		if (mill) {
			for (size_t i = 0; i < food_state->reseeds(player_id); ++i) {
				queue.items.push_back({"ReseedFarm", "Feld (neu einsäen)", "farm", false});
			}
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
			const auto *entry = known(head.name, head.research);
			Order order;
			order.building = id;
			order.building_label = label_of_entity(entity);
			order.label = head.research ? label_of(head.name, true) : unit_label(player_id, head.fqon);
			order.icon = entry ? entry->icon : (head.research ? "tech" : "sword");
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

	// XR fork W1: age, civilisation and the names of upgraded unit lines of the HUD player
	snap.age = hud_age;
	snap.age_label = tech::age_label(hud_age);
	if (const tech::PlayerTech *s = techs ? techs->find(player_id) : nullptr) {
		snap.civ = s->civ;
		snap.civ_label = s->civ.empty() ? std::string{} : civ_label(s->civ);
		std::set<std::string> seen;
		for (const auto &[id, entity] : entities) {
			if (not is_unit(entity) or not entity->has_component(component::component_t::OWNERSHIP)
			    or owner_of(entity, now) != player_id) {
				continue;
			}
			auto fqon = fqon_of_entity(entity);
			if (fqon.empty() or not seen.insert(fqon).second) {
				continue;
			}
			auto label = techs->unit_label(player_id, fqon);
			if (label != label_of(short_name(fqon))) {
				snap.unit_labels.emplace_back(short_name(fqon), label);
			}
		}
		std::sort(snap.unit_labels.begin(), snap.unit_labels.end());
	}

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
	auto place = check_place(state, builder_entity, *buildable, *cursor, now);
	coord::phys3 anchor{coord::phys_t{place.ne}, coord::phys_t{place.se}, coord::phys_t{0.0}};
	auto on_ground = map->on_terrain(anchor);
	result.anchor_ne = place.ne;
	result.anchor_se = place.se;
	result.anchor_up = on_ground.up.to_double();
	result.valid = place.ok;
	result.reason = result.valid ? std::string{} : place.message;
	// XR fork W3 (spec 13): buildings without an own picture (farm, trap) show only a white outline
	result.outline_only = is_field_name(buildable->name);

	std::lock_guard<std::mutex> lock{this->mutex};
	if (this->placement == id) {
		this->preview = std::move(result);
	}
}

} // namespace openage::gamestate::prod
