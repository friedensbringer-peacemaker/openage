// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "special.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include <eigen3/Eigen/Dense>

#include "event/event.h"
#include "event/event_loop.h"
#include "log/log.h"
#include "log/message.h"

#include "coord/phys.h"
#include "coord/tile.h"
#include "gamestate/api/ability.h"
#include "gamestate/api/animation.h"
#include "gamestate/api/property.h"
#include "gamestate/building_place.h"
#include "gamestate/buildings.h"
#include "gamestate/combat/behavior.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/rules.h"
#include "gamestate/combat/stats.h"
#include "gamestate/component/api/constructable.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/api/move.h"
#include "gamestate/component/api_component.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/move.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"
#include "gamestate/pick.h"
#include "gamestate/player.h"
#include "gamestate/production.h"
#include "gamestate/production_math.h"
#include "gamestate/resources.h"
#include "gamestate/tech.h"


namespace openage::gamestate::special {

namespace {

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
	return it == entities.end() ? nullptr : it->second;
}

combat::Point ground_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto pos = component_of<component::Position>(entity, component::component_t::POSITION);
	if (pos == nullptr) {
		return {0.0, 0.0};
	}
	auto p = pos->get_positions().get(time);
	return {p.ne.to_double(), p.se.to_double()};
}

coord::phys3 phys_of(double x, double y) {
	return coord::phys3{coord::phys_t{x}, coord::phys_t{y}, coord::phys_t{0.0}};
}

std::optional<player_id_t> owner_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto owner = component_of<component::Ownership>(entity, component::component_t::OWNERSHIP);
	if (owner == nullptr) {
		return std::nullopt;
	}
	return owner->get_owners().get(time);
}

/// nyan game entity of an entity, from the names of its abilities
std::string fqon_of(const std::shared_ptr<GameEntity> &entity) {
	for (auto type : {component::component_t::IDLE,
	                  component::component_t::SELECTABLE,
	                  component::component_t::LIVE,
	                  component::component_t::CONSTRUCTABLE,
	                  component::component_t::HARVESTABLE}) {
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

std::string short_of(const std::string &fqon) {
	auto dot = fqon.rfind('.');
	return dot == std::string::npos ? fqon : fqon.substr(dot + 1);
}

std::string object_name(const nyan::ValueHolder &value) {
	auto obj = std::dynamic_pointer_cast<nyan::ObjectValue>(value.get_ptr());
	return obj ? obj->get_name() : std::string{};
}

bool has_parent(const nyan::Object &obj, const std::string &part) {
	for (const auto &p : obj.get_parents()) {
		if (p.find(part) != std::string::npos) {
			return true;
		}
	}
	return false;
}

/// Idle and Move graphics of a carrier: EntityContainer.carry_progress ->
/// Progress.properties[Animated].overrides[*] (AnimationOverride.ability, .animations)
void carry_animations_of(const std::shared_ptr<nyan::View> &view, const nyan::Object &container, SpecialType &t) {
	try {
		for (const auto &progress_value : container.get_set("EntityContainer.carry_progress")) {
			auto progress = view->get_object(object_name(progress_value));
			for (const auto &[key, value] : progress.get_dict("Progress.properties")) {
				if (object_name(key) != "engine.util.progress.property.type.Animated") {
					continue;
				}
				auto animated = view->get_object(object_name(value));
				for (const auto &override_value : animated.get_set("Animated.overrides")) {
					auto override_obj = view->get_object(object_name(override_value));
					auto ability = view->get_object(override_obj.get_object("AnimationOverride.ability").get_name());
					for (const auto &animation_value : override_obj.get_set("AnimationOverride.animations")) {
						auto path = api::APIAnimation::get_animation_path(view->get_object(object_name(animation_value)));
						if (has_parent(ability, "engine.ability.type.Idle")) {
							t.carry_idle_animation = path;
						}
						else if (has_parent(ability, "engine.ability.type.Move")) {
							t.carry_move_animation = path;
						}
					}
				}
			}
		}
	}
	catch (const std::exception &err) {
		log::log(WARN << "Special: carry graphics of " << t.name << " not usable: " << err.what());
	}
}

/// Standing and death graphic of the unpacked form: GenieUnit.form_overrides[*] (AnimationOverride.ability, .animations).
/// Modpacks before tcfix6 do not have the member: the trebuchet then keeps its packed graphic.
void form_animations_of(const std::shared_ptr<nyan::View> &view, const nyan::Object &genie_unit, SpecialType &t) {
	try {
		for (const auto &override_value : genie_unit.get_orderedset("GenieUnit.form_overrides")) {
			auto override_obj = view->get_object(object_name(override_value));
			auto ability = view->get_object(override_obj.get_object("AnimationOverride.ability").get_name());
			for (const auto &animation_value : override_obj.get_set("AnimationOverride.animations")) {
				const auto path = api::APIAnimation::get_animation_path(view->get_object(object_name(animation_value)));
				if (has_parent(ability, "engine.ability.type.Idle")) {
					t.unpacked_idle_animation = path;
				}
				else if (has_parent(ability, "engine.ability.type.PassiveTransformTo")) {
					t.unpacked_death_animation = path;
				}
			}
		}
	}
	catch (const std::exception &err) {
		log::log(DBG << "Special: no form graphics for " << t.name << ": " << err.what());
	}
}

/// first animation of an Animated ability (empty if none)
std::string ability_animation(const std::shared_ptr<GameEntity> &entity, component::component_t kind) {
	if (not entity->has_component(kind)) {
		return {};
	}
	auto component = std::dynamic_pointer_cast<component::APIComponent>(entity->get_component(kind));
	if (component == nullptr) {
		return {};
	}
	auto ability = component->get_ability();
	if (not api::APIAbility::check_property(ability, api::ability_property_t::ANIMATED)) {
		return {};
	}
	auto property = api::APIAbility::get_property(ability, api::ability_property_t::ANIMATED);
	auto paths = api::APIAnimation::get_animation_paths(api::APIAbilityProperty::get_animations(property));
	return paths.empty() ? std::string{} : paths[0];
}

double float_or(const nyan::Object &obj, const std::string &member, double fallback) {
	try {
		return obj.get_float(member);
	}
	catch (std::exception &) {
	}
	try {
		return static_cast<double>(obj.get_int(member));
	}
	catch (std::exception &) {
	}
	return fallback;
}

int64_t int_or(const nyan::Object &obj, const std::string &member, int64_t fallback) {
	try {
		return obj.get_int(member);
	}
	catch (std::exception &) {
		return fallback;
	}
}

/// rate of an AttributeRate member (FlatAttributeChange.change_rate, RegenerateAttribute.rate)
double rate_of(const std::shared_ptr<nyan::View> &view, const nyan::Object &obj, const std::string &member) {
	try {
		auto rate = obj.get_object(member);
		return float_or(view->get_object(rate.get_name()), "AttributeRate.rate", 0.0);
	}
	catch (std::exception &) {
		return 0.0;
	}
}

std::shared_ptr<nyan::View> view_of(const std::shared_ptr<GameState> &state, std::optional<player_id_t> owner) {
	if (owner and state->has_player(*owner)) {
		return state->get_player(*owner)->get_db_view();
	}
	return state->get_db_view();
}

combat::Footprint footprint_of(const std::shared_ptr<GameEntity> &entity, const combat::CombatStats &stats,
                               const time::time_t &time) {
	return {ground_of(entity, time), stats.radius, stats.building};
}

bool is_moving(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto a = ground_of(entity, time);
	auto b = ground_of(entity, time + time::time_t::from_double(0.1));
	return combat::distance(a, b) > 1e-3;
}

std::optional<path::grid_id_t> grid_of(const std::shared_ptr<GameState> &state,
                                       const std::shared_ptr<GameEntity> &entity) {
	if (auto move = component_of<component::Move>(entity, component::component_t::MOVE)) {
		try {
			auto path_type = move->get_ability().get<nyan::ObjectValue>("Move.path_type");
			return state->get_map()->get_grid_id(path_type->get_name());
		}
		catch (std::exception &) {
		}
	}
	return std::nullopt;
}

bool finished(const std::shared_ptr<GameEntity> &entity) {
	auto constructable = component_of<component::Constructable>(entity, component::component_t::CONSTRUCTABLE);
	return constructable == nullptr or constructable->is_complete();
}

void move_to(const std::shared_ptr<GameState> &state, const std::shared_ptr<GameEntity> &entity, double x, double y,
             const time::time_t &time) {
	if (auto queue = component_of<component::CommandQueue>(entity, component::component_t::COMMANDQUEUE)) {
		queue->add_command(time, std::make_shared<component::command::MoveCommand>(
									 state->get_map()->on_terrain(phys_of(x, y))));
	}
}

std::string fmt1(double v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1f", v);
	return buf;
}

std::string describe(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto owner = owner_of(entity, time);
	return prod::entity_name(entity) + " " + std::to_string(entity->get_id())
	       + (owner ? " (P" + std::to_string(*owner) + ")" : std::string{});
}

/// the scout and eagle warrior lines (05 §5.7)
bool scout_unit(int unit_id) {
	switch (unit_id) {
	case 448:
	case 546:
	case 441:
	case 751:
	case 752:
	case 753:
		return true;
	default:
		return false;
	}
}

bool ship_class(int unit_class) {
	return unit_class == 2 or unit_class == 20 or unit_class == 21 or unit_class == 22 or unit_class == 53;
}

bool siege_class(int unit_class) {
	return unit_class == 13 or unit_class == 51 or unit_class == 54 or unit_class == 55;
}

/// read the special values of a game entity type in a view
SpecialType read_type(const std::shared_ptr<nyan::View> &view, const nyan::fqon_t &fqon) {
	SpecialType t;
	t.name = short_of(fqon);
	nyan::Object obj = view->get_object(fqon);
	for (const auto &value : obj.get_set("GameEntity.types")) {
		const auto type = short_of(object_name(value));
		if (type == "Unit") {
			t.unit = true;
		}
		else if (type == "Building") {
			t.building = true;
		}
		else if (type == "SiegeWeapon" or type == "Trebuchet") {
			t.siege = true;
		}
		else if (type.find("Ship") != std::string::npos) {
			t.ship = true;
		}
		else if (type == "Relic") {
			t.relic = true;
		}
	}
	int heal_task_found = 0;
	for (const auto &value : obj.get_set("GameEntity.abilities")) {
		const auto name = object_name(value);
		if (name.empty()) {
			continue;
		}
		nyan::Object ability = view->get_object(name);
		const auto ability_name = short_of(name);
		try {
			if (has_parent(ability, "RangedDiscreteEffect") and ability_name == "Convert") {
				t.monk = true;
				t.convert_range = float_or(ability, "RangedDiscreteEffect.max_range", t.convert_range);
				t.convert_reload = std::max(0.1, float_or(ability, "ApplyDiscreteEffect.reload_time", t.convert_reload));
				t.convert_units = false;
				for (const auto &allowed : ability.get_set("ApplyDiscreteEffect.allowed_types")) {
					const auto type = short_of(object_name(allowed));
					t.convert_units = t.convert_units or type == "Unit";
					t.convert_buildings = t.convert_buildings or type == "Building";
				}
				for (const auto &black : ability.get_set("ApplyDiscreteEffect.blacklisted_entities")) {
					t.convert_blacklist.push_back(object_name(black));
				}
				for (const auto &batch_value : ability.get_set("ApplyDiscreteEffect.batches")) {
					auto batch = view->get_object(object_name(batch_value));
					for (const auto &effect_value : batch.get_set("EffectBatch.effects")) {
						auto effect = view->get_object(object_name(effect_value));
						if (has_parent(effect, "Convert")) {
							const double chance = float_or(effect, "Convert.chance_success", -1.0);
							if (chance >= 0.0) {
								t.convert_chance = chance * 100.0;
								break;
							}
						}
					}
				}
			}
			else if (has_parent(ability, "RangedContinuousEffect") and ability_name == "Heal") {
				t.heals = true;
				t.heal_range = float_or(ability, "RangedContinuousEffect.max_range", t.heal_range);
				for (const auto &effect_value : ability.get_set("ApplyContinuousEffect.effects")) {
					auto effect = view->get_object(object_name(effect_value));
					const double rate = rate_of(view, effect, "FlatAttributeChange.change_rate");
					if (rate > 0.0 and heal_task_found == 0) {
						t.heal_task = rate;
					}
				}
			}
			else if (has_parent(ability, "RegenerateAttribute") and ability_name == "RegenerateFaith") {
				t.faith_rate = rate_of(view, ability, "RegenerateAttribute.rate");
			}
			else if (has_parent(ability, "GameEntityStance")) {
				for (const auto &stance_value : ability.get_set("GameEntityStance.stances")) {
					auto stance = view->get_object(object_name(stance_value));
					t.search_range = std::max(t.search_range, float_or(stance, "GameEntityStance.search_range", 0.0));
				}
			}
			else if (has_parent(ability, "engine.ability.type.ApplyDiscreteEffect") and ability_name == "Attack") {
				t.melee = true;
			}
			else if (has_parent(ability, "CollectStorage")) {
				t.carries_relics = true;
			}
			else if (has_parent(ability, "engine.ability.type.Storage")) {
				auto container = view->get_object(ability.get_object("Storage.container").get_name());
				t.cargo_slots = std::max(t.cargo_slots, static_cast<int>(int_or(container, "EntityContainer.slots", 0)));
				carry_animations_of(view, container, t);
			}
			else if (has_parent(ability, "engine.xr.ability.GenieUnit")) {
				t.unit_class = static_cast<int>(int_or(ability, "GenieUnitData.unit_class", -1));
				t.unit_id = static_cast<int>(int_or(ability, "GenieUnitData.unit_id", -1));
				t.unit_level = static_cast<int>(int_or(ability, "GenieUnitData.unit_level", -1));
				t.work_rate = float_or(ability, "GenieUnitData.work_rate", t.work_rate);
				t.blast_range = std::max(t.blast_range, float_or(ability, "GenieUnitData.blast_range", 0.0));
				t.blast_offence = static_cast<int>(int_or(ability, "GenieUnitData.blast_level_offence", 0));
				t.blast_defense = static_cast<int>(int_or(ability, "GenieUnitData.blast_defense_level", 0));
				const int capacity = static_cast<int>(int_or(ability, "GenieUnitData.garrison_capacity", 0));
				// the trebuchet: the head unit (packed, class 51) has the unpacked one as its transform unit
				const int transform = static_cast<int>(int_or(ability, "GenieUnitData.transform_unit_id", -1));
				if (transform >= 0 and t.unit_class == 51) {
					t.packable = true;
					form_animations_of(view, ability, t);
				}
				if (capacity > 0 and not t.building) {
					t.cargo_slots = std::max(t.cargo_slots, capacity);
				}
				for (const auto &task_value : ability.get_orderedset("GenieUnitData.tasks")) {
					auto task = view->get_object(object_name(task_value));
					const auto kind = int_or(task, "GenieTask.task_type", -1);
					if (kind == 104) {
						// conversion: work values 1/2 = min/max time (05 §5.7)
						t.convert_min = float_or(task, "GenieTask.work_value1", t.convert_min);
						t.convert_max = float_or(task, "GenieTask.work_value2", t.convert_max);
					}
					else if (kind == 105) {
						// healing: work value 1 = a₁ (05 §5.8)
						const double a1 = float_or(task, "GenieTask.work_value1", 0.0);
						if (a1 > 0.0) {
							t.heal_task = a1;
							heal_task_found = 1;
						}
					}
				}
			}
		}
		catch (std::exception &err) {
			log::log(DBG << "Special: ability " << name << " not read: " << err.what());
		}
	}
	if (t.unit_class >= 0) {
		t.ship = t.ship or ship_class(t.unit_class);
		t.siege = t.siege or siege_class(t.unit_class);
		t.relic = t.relic or t.unit_class == 42;
	}
	t.monastery = t.unit_id == 104 or t.name == "Monastery";
	t.scout_line = scout_unit(t.unit_id);
	t.never_converted = t.name == "TownCenter" or t.name == "Castle" or t.name == "Wonder" or t.name == "Farm"
	                    or t.name == "FishingTrap" or t.name.find("Wall") != std::string::npos
	                    or t.name.find("Gate") != std::string::npos or t.name.find("Palisade") != std::string::npos;
	// carriers: transport ships take land units, rams (siege class 13 with places) infantry
	if (t.cargo_slots > 0 and not t.building) {
		if (t.ship and (t.name.find("Transport") != std::string::npos or t.unit_class == 20)) {
			t.cargo = cargo_t::TRANSPORT;
		}
		else if (t.siege and t.unit_class == 13) {
			t.cargo = cargo_t::RAM;
		}
	}
	if (t.cargo == cargo_t::NONE and not t.monk) {
		t.cargo_slots = 0;
	}
	// demolition ships (class 22 with an area attack) and petards blow up with their attack
	t.self_destruct = t.name == "Petard" or t.name.find("DemolitionShip") != std::string::npos
	                  or t.name.find("DemolitionRaft") != std::string::npos;
	t.ram = t.siege and t.melee and t.unit_class == 13;
	if (t.search_range <= 0.0) {
		t.search_range = t.heal_range + 5.0;
	}
	return t;
}

} // namespace


SpecialState::SpecialState(const std::shared_ptr<openage::event::EventLoop> &loop) :
	EventEntity{loop},
	loop{loop},
	handler{std::make_shared<SpecialEventHandler>()} {
}

size_t SpecialState::id() const {
	// fixed id, the special state exists once per game ("spec")
	return 0x73706563;
}

std::string SpecialState::idstr() const {
	return "special";
}

void SpecialState::start(const std::shared_ptr<GameState> &state,
                         const std::unordered_set<player_id_t> &neutral,
                         uint64_t seed,
                         const time::time_t &time) {
	if (this->started) {
		return;
	}
	this->started = true;
	this->neutral = neutral;
	this->rng.seed(0x57365ull ^ (seed * 0x9E3779B97F4A7C15ull));
	this->last_tick = time.to_double();
	std::weak_ptr<GameState> weak_state = state;
	std::weak_ptr<SpecialState> weak_self = this->shared_from_this();
	auto combat = state->get_combat();
	combat->on_death([weak_state, weak_self](const std::shared_ptr<GameEntity> &entity, const time::time_t &t) {
		auto s = weak_state.lock();
		auto self = weak_self.lock();
		return s and self and self->on_death(s, entity, t);
	});
	combat->on_impact([weak_state, weak_self](entity_id_t attacker, player_id_t owner, const combat::CombatStats &stats,
	                                          std::optional<entity_id_t> main_target, double x0, double y0,
	                                          const time::time_t &t) {
		auto s = weak_state.lock();
		auto self = weak_self.lock();
		if (s and self) {
			self->on_impact(s, attacker, owner, stats, main_target, x0, y0, t);
		}
	});
	// rams attack buildings only (spec 05 §5.5 "Rammen: nur Gebäude"): orders and automatic targets
	combat->on_siege_gate([weak_state, weak_self](entity_id_t id, bool in_range, bool explicit_order, const time::time_t &t) {
		auto s = weak_state.lock();
		auto self = weak_self.lock();
		if (not s or not self) {
			return combat::CombatState::siege_gate_t::GO;
		}
		return self->siege_gate(s, id, in_range, explicit_order, t);
	});
	combat->on_target([weak_state, weak_self](entity_id_t attacker, entity_id_t target) {
		auto s = weak_state.lock();
		auto self = weak_self.lock();
		if (not s or not self or not s->get_game_entities().contains(attacker)) {
			return true;
		}
		if (not self->ram_ids.contains(attacker)) {
			const auto *t = self->type_of(s, s->get_game_entity(attacker), time::time_t::from_double(self->last_tick));
			if (t == nullptr or not t->ram) {
				return true;
			}
			self->ram_ids.insert(attacker);
		}
		auto ts = s->get_combat()->get_stats(target);
		return ts != nullptr and ts->building;
	});
	this->discover(state, time);
	size_t relics = 0;
	size_t monks = 0;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (auto t = this->type_of(state, entity, time)) {
			relics += t->relic ? 1 : 0;
			monks += t->monk ? 1 : 0;
		}
	}
	log::log(INFO << "Special: started, " << relics << " relics, " << monks << " monks (tick " << TICK << " s)");
	this->tick(state, time);
}

bool SpecialState::is_started() const {
	return this->started;
}

SpecialState::Unit &SpecialState::unit(entity_id_t id) {
	return this->units[id];
}

SpecialState::Unit *SpecialState::find_unit(entity_id_t id) {
	auto it = this->units.find(id);
	return it == this->units.end() ? nullptr : &it->second;
}

const SpecialState::Unit *SpecialState::find_unit(entity_id_t id) const {
	auto it = this->units.find(id);
	return it == this->units.end() ? nullptr : &it->second;
}

const SpecialType *SpecialState::type_of(const std::shared_ptr<GameState> &state,
                                         const std::shared_ptr<GameEntity> &entity,
                                         const time::time_t &time) {
	if (entity == nullptr) {
		return nullptr;
	}
	const auto fqon = fqon_of(entity);
	if (fqon.empty()) {
		return nullptr;
	}
	auto view = view_of(state, owner_of(entity, time));
	std::string key = fqon + "@" + std::to_string(reinterpret_cast<uintptr_t>(view.get()));
	auto it = this->types.find(key);
	if (it == this->types.end()) {
		SpecialType t;
		try {
			t = read_type(view, fqon);
		}
		catch (std::exception &err) {
			log::log(DBG << "Special: type " << fqon << " not read: " << err.what());
			t.name = short_of(fqon);
		}
		if (t.monk or t.cargo != cargo_t::NONE or t.packable or t.blast_range > 0.0) {
			log::log(INFO << "Special: type " << t.name << " class " << t.unit_class << (t.monk ? " monk" : "")
			              << (t.monk ? " convert range " + fmt1(t.convert_range) + " chance " + fmt1(t.convert_chance)
			                               + " time " + fmt1(t.convert_min) + "-" + fmt1(t.convert_max) + " s" :
			                           std::string{})
			              << (t.heals ? " heal range " + fmt1(t.heal_range) + " a1 " + fmt1(t.heal_task) : std::string{})
			              << (t.faith_rate > 0.0 ? " faith " + fmt1(t.faith_rate) + "/s" : std::string{})
			              << (t.cargo != cargo_t::NONE ? " cargo " + std::to_string(t.cargo_slots) : std::string{})
			              << (t.packable ? " packable" : "")
			              << (t.blast_range > 0.0 ? " blast " + fmt1(t.blast_range) + " level " + std::to_string(t.blast_offence) :
			                                        std::string{})
			              << (t.self_destruct ? " self-destruct" : ""));
		}
		it = this->types.emplace(key, std::move(t)).first;
	}
	return &it->second;
}

double SpecialState::player_resource(const std::shared_ptr<GameState> &state, player_id_t player, int index,
                                     double fallback) {
	auto &cache = this->civ_resources[player];
	if (cache.empty()) {
		cache[-1] = 0.0;  // looked up (also when nothing is found)
		std::string civ;
		if (auto tech = state->get_tech()) {
			if (const auto *p = tech->find(player)) {
				civ = p->civ;
			}
		}
		try {
			auto view = view_of(state, player);
			std::string modpack = "hd_base";
			for (const auto &[id, entity] : state->get_game_entities()) {
				auto fqon = fqon_of(entity);
				if (not fqon.empty() and fqon.compare(0, 6, "engine") != 0) {
					modpack = fqon.substr(0, fqon.find('.'));
					break;
				}
			}
			nyan::Object data = view->get_object(modpack + ".data.util.xr.genie.GenieData");
			for (const auto &civ_value : data.get_orderedset("GenieData.civs")) {
				auto c = view->get_object(object_name(civ_value));
				std::string setup;
				try {
					setup = short_of(c.get_object("GenieCiv.setup").get_name());
				}
				catch (std::exception &) {
				}
				const bool match = civ.empty() ? int_or(c, "GenieCiv.civ_id", -1) == 0 : setup == civ;
				if (not match) {
					continue;
				}
				for (const auto &[k, v] : c.get_dict("GenieCiv.resources")) {
					auto key = std::dynamic_pointer_cast<nyan::Int>(k.get_ptr());
					auto fl = std::dynamic_pointer_cast<nyan::Float>(v.get_ptr());
					if (key and fl) {
						cache[static_cast<int>(key->get())] = fl->get();
					}
				}
				log::log(INFO << "Special: player " << player << " resources of civilisation "
				              << (civ.empty() ? std::string{"Gaia (none chosen)"} : civ) << ": faith rate "
				              << fmt1(cache.contains(35) ? cache[35] : -1.0) << ", relic gold "
				              << fmt1(cache.contains(191) ? cache[191] : -1.0));
				break;
			}
		}
		catch (std::exception &err) {
			log::log(INFO << "Special: no player resources in the data (" << err.what() << "), spec values used");
		}
	}
	auto it = cache.find(index);
	return it == cache.end() ? fallback : it->second;
}

bool SpecialState::researched(const std::shared_ptr<GameState> &state, player_id_t player, const std::string &tech) const {
	auto techs = state->get_tech();
	if (techs == nullptr) {
		return false;
	}
	const auto *p = techs->find(player);
	if (p == nullptr) {
		return false;
	}
	for (const auto &fqon : p->order) {
		if (short_of(fqon) == tech) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------- tick

void SpecialState::discover(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	auto combat = state->get_combat();
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (this->seen.contains(id)) {
			continue;
		}
		this->seen.insert(id);
		const auto *t = this->type_of(state, entity, time);
		if (t == nullptr) {
			continue;
		}
		if (t->relic) {
			this->relic_ids.insert(id);
		}
		if (t->ram) {
			this->ram_ids.insert(id);
		}
		if (t->monk or t->faith_rate > 0.0 or t->packable) {
			auto &u = this->unit(id);
			u.has_faith = t->faith_rate > 0.0 or t->monk;
		}
	}
	// gone entities
	std::erase_if(this->units, [&](const auto &item) {
		return not state->get_game_entities().contains(item.first);
	});
}

void SpecialState::tick(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	if (not this->started) {
		return;
	}
	const double now = time.to_double();
	const double dt = std::clamp(now - this->last_tick, 0.0, 5.0);
	this->last_tick = now;
	auto combat = state->get_combat();
	combat->note_time(time);
	this->discover(state, time);

	std::vector<entity_id_t> ids;
	ids.reserve(this->units.size());
	for (const auto &[id, u] : this->units) {
		ids.push_back(id);
	}
	std::sort(ids.begin(), ids.end());
	for (auto id : ids) {
		auto *u = this->find_unit(id);
		auto entity = find_entity(state, id);
		if (u == nullptr or entity == nullptr or combat->is_dead(id)) {
			continue;
		}
		const auto *t = this->type_of(state, entity, time);
		auto owner = owner_of(entity, time);
		// faith (05 §5.7): player resource 35 per second, Illumination sets it to 3
		if (u->has_faith and owner and u->faith < 100.0) {
			double rate = this->player_resource(state, *owner, 35, t ? t->faith_rate : 1.6);
			if (rate <= 0.0) {
				rate = t and t->faith_rate > 0.0 ? t->faith_rate : 1.6;
			}
			if (this->researched(state, *owner, "Illumination")) {
				rate = std::max(rate, 3.0);
			}
			u->faith = faith_after(u->faith, rate, dt);
		}
		// trebuchet: packing and unpacking, no movement while unpacked
		if (t and t->packable) {
			if (u->pack_until and now >= *u->pack_until) {
				u->pack_until.reset();
				u->unpacked = not u->pack_target;
				this->stats.packs += 1;
				this->show_form(state, id, u->unpacked, time);
				log::log(INFO << "Special: " << describe(entity, time) << (u->unpacked ? " unpacked" : " packed")
				              << " at t=" << fmt1(now));
				if (not u->unpacked and u->after_pack) {
					auto dest = *u->after_pack;
					u->after_pack.reset();
					move_to(state, entity, dest.ne.to_double(), dest.se.to_double(), time);
				}
			}
			if ((u->unpacked or u->pack_until) and is_moving(entity, time)) {
				combat->halt_entity(id, time);
			}
		}
		if (combat->garrisoned_in(id)) {
			continue;
		}
		this->run_job(state, id, *u, dt, time);
	}

	this->relic_gold(state, time);

	// cargo/relics of entities that are gone (removed without a death)
	for (auto it = this->carried.begin(); it != this->carried.end();) {
		if (not state->get_game_entities().contains(it->first)) {
			for (auto inner : it->second) {
				this->inside.erase(inner);
			}
			it = this->carried.erase(it);
		}
		else {
			++it;
		}
	}

	if (now >= this->last_log + 30.0) {
		this->last_log = now;
		const auto &c = this->stats;
		log::log(INFO << "Special: " << c.conversions << " conversions (" << c.conversion_attempts << " attempts, "
		              << c.heresy_deaths << " heresy deaths), " << std::lround(c.healed) << " HP healed, relics "
		              << c.relics_picked << " picked/" << c.relics_delivered << " delivered/" << c.relics_dropped
		              << " dropped, " << fmt1(c.relic_gold) << " relic gold, " << c.splash_hits << " area hits ("
		              << c.friendly_hits << " own), cargo " << c.boarded << " in/" << c.unloaded << " out/"
		              << c.cargo_lost << " lost, " << c.packs << " packs, t=" << fmt1(now) << " s");
	}
	this->publish(state, time);
	this->loop->create_event(this->handler,
	                         this->shared_from_this(),
	                         state,
	                         time + time::time_t::from_double(TICK),
	                         openage::event::EventHandler::param_map::map_t{});
}

bool SpecialState::approach(const std::shared_ptr<GameState> &state, entity_id_t id, Job &job, entity_id_t target,
                            double reach, const time::time_t &time) {
	auto combat = state->get_combat();
	auto self = find_entity(state, id);
	auto other = find_entity(state, target);
	auto as = combat->get_stats(id);
	auto ts = combat->get_stats(target);
	if (self == nullptr or other == nullptr or as == nullptr or ts == nullptr) {
		return false;
	}
	const auto fa = footprint_of(self, *as, time);
	const auto ft = footprint_of(other, *ts, time);
	const double edge = combat::edge_distance(fa, ft);
	const double now = time.to_double();
	if (edge <= reach) {
		if (job.move_dest) {
			job.move_dest.reset();
			combat->halt_entity(id, time);
		}
		job.failures = 0;
		return true;
	}
	if (not as->movable) {
		job.failures = 1000;
		return false;
	}
	const bool moving = is_moving(self, time);
	if (job.move_dest and now - job.moved_at < 0.5) {
		return false;
	}
	if (job.move_dest and not moving and now - job.moved_at > 1.0) {
		job.failures += 1;
	}
	const auto dest = combat::approach_point(fa.center, as->radius, ft, std::max(0.1, reach * 0.7),
	                                         0.785398 * job.failures);
	if (job.move_dest and moving
	    and combat::distance(dest, combat::Point{job.move_dest->first, job.move_dest->second}) < 0.75) {
		return false;
	}
	move_to(state, self, dest.x, dest.y, time);
	job.move_dest = std::pair{dest.x, dest.y};
	job.moved_at = now;
	return false;
}

void SpecialState::run_job(const std::shared_ptr<GameState> &state, entity_id_t id, Unit &u, double dt,
                           const time::time_t &time) {
	auto combat = state->get_combat();
	auto entity = find_entity(state, id);
	const auto *t = this->type_of(state, entity, time);
	auto end = [&](const std::string &why) {
		if (u.job.kind != job_t::NONE and not why.empty()) {
			log::log(INFO << "Special: " << describe(entity, time) << " ends its job: " << why);
		}
		u.job = Job{};
	};
	if (u.job.failures > 12) {
		end("target not reachable");
		return;
	}
	switch (u.job.kind) {
	case job_t::NONE:
		if (t and t->heals and not u.relic and not is_moving(entity, time)
		    and combat->get_stance(id) != combat::stance_t::NO_ATTACK and time.to_double() - u.last_auto >= 1.0) {
			u.last_auto = time.to_double();
			this->auto_heal(state, id, u, time);
		}
		return;
	case job_t::CONVERT: {
		auto reason = this->convert_check(state, id, u.job.target, time);
		if (not reason.empty()) {
			end(reason);
			return;
		}
		if (not this->approach(state, id, u.job, u.job.target, t ? t->convert_range : 9.0, time)) {
			return;
		}
		const double now = time.to_double();
		if (not u.job.prayer_since) {
			u.job.prayer_since = now;
			u.job.next_attempt = now + (t ? t->convert_reload : 1.0);
			log::log(INFO << "Special: " << describe(entity, time) << " prays for " << u.job.target
			              << " (faith " << fmt1(u.faith) << ") at t=" << fmt1(now));
		}
		while (u.job.kind == job_t::CONVERT and now + 1e-9 >= u.job.next_attempt) {
			u.job.next_attempt += t ? t->convert_reload : 1.0;
			this->attempt_conversion(state, id, u, time);
		}
		return;
	}
	case job_t::HEAL: {
		auto target = find_entity(state, u.job.target);
		auto ts = combat->get_stats(u.job.target);
		auto tt = this->type_of(state, target, time);
		auto live = component_of<component::Live>(target, component::component_t::LIVE);
		if (target == nullptr or ts == nullptr or tt == nullptr or live == nullptr or combat->is_dead(u.job.target)
		    or combat->garrisoned_in(u.job.target) or owner_of(target, time) != owner_of(entity, time)) {
			end("heal target gone");
			return;
		}
		auto health = live->get_attribute(time, ts->health_attribute);
		if (not health or *health >= ts->max_health) {
			end(u.job.explicit_order ? "healed" : "");
			return;
		}
		double range = t ? t->heal_range : 4.0;
		if (auto owner = owner_of(entity, time)) {
			const double r90 = this->player_resource(state, *owner, 90, 0.0);
			range = r90 > 0.0 ? r90 : range;
		}
		if (not this->approach(state, id, u.job, u.job.target, range, time)) {
			return;
		}
		this->heal_step(state, id, u, dt, time);
		return;
	}
	case job_t::REPAIR: {
		auto target = find_entity(state, u.job.target);
		auto ts = combat->get_stats(u.job.target);
		auto tt = this->type_of(state, target, time);
		auto live = component_of<component::Live>(target, component::component_t::LIVE);
		if (target == nullptr or ts == nullptr or tt == nullptr or live == nullptr or combat->is_dead(u.job.target)
		    or combat->garrisoned_in(u.job.target) or owner_of(target, time) != owner_of(entity, time)) {
			end("repair target gone");
			return;
		}
		auto health = live->get_attribute(time, ts->health_attribute);
		if (not health or *health >= ts->max_health) {
			end("repaired");
			return;
		}
		// a ship lies off the shore: own assumption, the data have no repair range
		if (not this->approach(state, id, u.job, u.job.target, tt->ship ? 1.5 : 0.6, time)) {
			return;
		}
		if (not this->repair_step(state, id, u, dt, time)) {
			end("owner cannot pay the repair");
		}
		return;
	}
	case job_t::PICKUP: {
		auto relic = find_entity(state, u.job.target);
		if (relic == nullptr or this->inside.contains(u.job.target) or u.relic) {
			end("relic gone or taken");
			return;
		}
		if (not this->approach(state, id, u.job, u.job.target, 0.4, time)) {
			return;
		}
		this->stow_into(state, u.job.target, id, time);
		u.relic = u.job.target;
		this->show_carrying(state, id, true, time);
		this->stats.relics_picked += 1;
		log::log(INFO << "Special: " << describe(entity, time) << " picks up relic " << u.job.target << " at t="
		              << fmt1(time.to_double()));
		end("");
		return;
	}
	case job_t::DELIVER: {
		auto monastery = find_entity(state, u.job.target);
		if (monastery == nullptr or not u.relic or combat->is_dead(u.job.target)
		    or owner_of(monastery, time) != owner_of(entity, time)) {
			end("monastery gone");
			return;
		}
		if (not this->approach(state, id, u.job, u.job.target, 0.5, time)) {
			return;
		}
		const auto relic = *u.relic;
		this->release(state, relic, 0, false, time);
		this->stow_into(state, relic, u.job.target, time);
		u.relic.reset();
		this->show_carrying(state, id, false, time);
		this->stats.relics_delivered += 1;
		log::log(INFO << "Special: " << describe(entity, time) << " brings relic " << relic << " into "
		              << describe(monastery, time) << " (" << this->stowed(u.job.target).size() << " relics) at t="
		              << fmt1(time.to_double()));
		end("");
		return;
	}
	case job_t::BOARD: {
		auto reason = this->board_check(state, id, u.job.target, time);
		if (not reason.empty()) {
			end(reason);
			return;
		}
		auto carrier_type = this->type_of(state, find_entity(state, u.job.target), time);
		const double reach = carrier_type and carrier_type->ship ? 2.0 : 0.6;
		if (not this->approach(state, id, u.job, u.job.target, reach, time)) {
			if (time.to_double() - u.job.started > 30.0) {
				end("carrier not reached");
			}
			return;
		}
		this->stow_into(state, id, u.job.target, time);
		this->stats.boarded += 1;
		log::log(INFO << "Special: " << describe(entity, time) << " boards " << u.job.target << " ("
		              << this->stowed(u.job.target).size() << "/" << (carrier_type ? carrier_type->cargo_slots : 0)
		              << ") at t=" << fmt1(time.to_double()));
		u.job = Job{};
		return;
	}
	case job_t::UNLOAD: {
		auto cargo = this->stowed(id);
		if (cargo.empty()) {
			end("");
			return;
		}
		// at the shore: let everybody out
		if (this->release(state, cargo.front(), 0, true, time)) {
			size_t out = 1;
			for (size_t i = 1; i < cargo.size(); ++i) {
				out += this->release(state, cargo[i], i, false, time) ? 1 : 0;
			}
			this->stats.unloaded += out;
			log::log(INFO << "Special: " << describe(entity, time) << " unloads " << out << " units at t="
			              << fmt1(time.to_double()));
			combat->halt_entity(id, time);
			end("");
			return;
		}
		const double now = time.to_double();
		if (now - u.job.started > 60.0) {
			end("no shore reached");
			return;
		}
		if (u.job.point and not is_moving(entity, time) and now - u.job.moved_at > 1.0) {
			// sail towards the point: the nearest tile the ship can reach next to it
			auto grid = grid_of(state, entity);
			auto map = state->get_map();
			const double px = u.job.point->ne.to_double();
			const double py = u.job.point->se.to_double();
			std::optional<combat::Point> best;
			double best_d = 1e18;
			const auto here = ground_of(entity, time);
			for (int r = 0; r <= 6 and not best; ++r) {
				for (int dx = -r; dx <= r; ++dx) {
					for (int dy = -r; dy <= r; ++dy) {
						if (std::max(std::abs(dx), std::abs(dy)) != r) {
							continue;
						}
						combat::Point p{std::floor(px) + dx + 0.5, std::floor(py) + dy + 0.5};
						if (p.x < 0.0 or p.y < 0.0) {
							continue;
						}
						if (grid and not map->is_passable(*grid, phys_of(p.x, p.y).to_tile())) {
							continue;
						}
						const double d = combat::distance(p, here) * 0.01 + combat::distance(p, {px, py});
						if (d < best_d) {
							best_d = d;
							best = p;
						}
					}
				}
			}
			if (best and combat::distance(*best, here) > 0.5) {
				move_to(state, entity, best->x, best->y, time);
				u.job.moved_at = now;
				u.job.failures += 1;
			}
			else if (u.job.failures > 0 or not best) {
				end("no shore near the point");
			}
		}
		return;
	}
	}
}

std::string SpecialState::convert_check(const std::shared_ptr<GameState> &state, entity_id_t monk, entity_id_t target,
                                        const time::time_t &time) {
	auto combat = state->get_combat();
	auto m = find_entity(state, monk);
	auto e = find_entity(state, target);
	if (m == nullptr or e == nullptr or combat->is_dead(target) or combat->garrisoned_in(target)) {
		return "Ziel nicht mehr da";
	}
	const auto *mt = this->type_of(state, m, time);
	const auto *tt = this->type_of(state, e, time);
	auto ts = combat->get_stats(target);
	if (mt == nullptr or not mt->monk) {
		return "kann nicht konvertieren";
	}
	if (tt == nullptr or ts == nullptr or not ts->attackable()) {
		return "nicht konvertierbar";
	}
	auto mo = owner_of(m, time);
	auto to = owner_of(e, time);
	if (not mo or not to or *mo == *to or this->neutral.contains(*to) or this->neutral.contains(*mo)) {
		return "kein gegnerisches Ziel";
	}
	if (tt->relic or ts->animal or ts->ambient) {
		return "nicht konvertierbar";
	}
	// buildings and siege need Redemption (resource 28), monks Atonement (resource 27), 05 §5.7 step 4
	const bool redemption = this->researched(state, *mo, "Redemption") or this->player_resource(state, *mo, 28, 0.0) > 0.0;
	const bool atonement = this->researched(state, *mo, "Atonement") or this->player_resource(state, *mo, 27, 0.0) > 0.0;
	if (ts->building) {
		if (tt->never_converted) {
			return "Gebäude dieser Art wird nie konvertiert";
		}
		if (not(mt->convert_buildings or redemption)) {
			return "Gebäude erst mit Erlösung";
		}
		return {};
	}
	if (not mt->convert_units) {
		return "nicht konvertierbar";
	}
	const auto fqon = fqon_of(e);
	if (std::find(mt->convert_blacklist.begin(), mt->convert_blacklist.end(), fqon) != mt->convert_blacklist.end()) {
		if (tt->monk) {
			return atonement ? std::string{} : "Mönche erst mit Sühne";
		}
		return redemption ? std::string{} : "Belagerung erst mit Erlösung";
	}
	if (tt->monk and not atonement) {
		return "Mönche erst mit Sühne";
	}
	if (tt->siege and not redemption) {
		return "Belagerung erst mit Erlösung";
	}
	return {};
}

void SpecialState::attempt_conversion(const std::shared_ptr<GameState> &state, entity_id_t monk, Unit &u,
                                      const time::time_t &time) {
	auto combat = state->get_combat();
	auto m = find_entity(state, monk);
	auto e = find_entity(state, u.job.target);
	const auto *mt = this->type_of(state, m, time);
	const auto *tt = this->type_of(state, e, time);
	auto mo = owner_of(m, time);
	auto to = owner_of(e, time);
	auto ts = combat->get_stats(u.job.target);
	if (mt == nullptr or tt == nullptr or not mo or not to or ts == nullptr) {
		return;
	}
	const double now = time.to_double();
	std::uniform_real_distribution<double> uniform{0.0, 100.0};
	ConvertParams p{};
	p.z = uniform(this->rng);
	p.target_ship = tt->ship;
	p.target_building_level = tt->unit_level == 2 or tt->unit_level == 10;
	p.target_scout = tt->scout_line;
	// resistance 77 of the target owner; the technology Faith adds 3 (05 §5.7)
	p.resist_77 = this->player_resource(state, *to, 77, 0.0) + (this->researched(state, *to, "Faith") ? 3.0 : 0.0);
	p.target_building_tower = ts->building or tt->unit_class == 3 or tt->unit_class == 52;
	p.res_182 = this->player_resource(state, *mo, 182, 25.0);
	p.res_180 = this->player_resource(state, *mo, 180, 15.0);
	p.res_181 = this->player_resource(state, *mo, 181, 25.0);
	p.accuracy = mt->convert_chance;
	p.work_min = mt->convert_min;
	p.work_max = mt->convert_max;
	p.res_178 = this->player_resource(state, *to, 178, 0.0);
	p.res_179 = this->player_resource(state, *to, 179, 0.0);
	p.res_176 = this->player_resource(state, *mo, 176, 0.0);
	p.res_177 = this->player_resource(state, *mo, 177, 0.0);
	p.t = now - u.job.prayer_since.value_or(now);
	this->stats.conversion_attempts += 1;
	const auto result = attempt(p);
	if (result != convert_t::SUCCESS) {
		return;
	}
	const auto target_id = u.job.target;
	const bool heresy = this->researched(state, *to, "Heresy") or this->player_resource(state, *to, 192, 0.0) > 0.0;
	const bool theocracy = this->researched(state, *mo, "Theocracy") or this->player_resource(state, *mo, 193, 0.0) > 0.0;
	log::log(INFO << "Special: " << describe(m, time) << " converts " << describe(e, time) << " after " << fmt1(p.t)
	              << " s (z " << fmt1(p.z) << ")" << (heresy ? " – heresy: it dies" : "") << " at t=" << fmt1(now));
	// faith of the monks working on this target (05 §5.7 step 3)
	for (auto &[other_id, other] : this->units) {
		if (other_id == monk or other.job.kind != job_t::CONVERT or other.job.target != target_id) {
			continue;
		}
		if (faith_reset_all(theocracy)) {
			other.faith = 0.0;
		}
		other.job = Job{};
	}
	u.faith = 0.0;
	u.job = Job{};
	if (not converts_or_dies(heresy)) {
		this->stats.heresy_deaths += 1;
		combat->kill(state, target_id, time);
		return;
	}
	// cargo of a converted carrier comes out first
	auto cargo = this->stowed(target_id);
	for (size_t i = 0; i < cargo.size(); ++i) {
		if (not tt->monastery) {
			this->release(state, cargo[i], i, false, time);
		}
	}
	if (auto *tu = this->find_unit(target_id)) {
		tu->job = Job{};
	}
	combat->change_owner(state, target_id, *mo, view_of(state, *mo), fqon_of(e), time);
	this->stats.conversions += 1;
}

bool SpecialState::repair_step(const std::shared_ptr<GameState> &state, entity_id_t villager, Unit &u, double dt,
                               const time::time_t &time) {
	auto combat = state->get_combat();
	auto v = find_entity(state, villager);
	auto target = find_entity(state, u.job.target);
	auto ts = combat->get_stats(u.job.target);
	auto live = component_of<component::Live>(target, component::component_t::LIVE);
	auto owner = owner_of(target, time);
	auto health = live and ts ? live->get_attribute(time, ts->health_attribute) : std::nullopt;
	if (v == nullptr or not owner or not health) {
		return true;
	}
	int workers = 0;
	for (const auto &[id, other] : this->units) {
		if (other.job.kind == job_t::REPAIR and other.job.target == u.job.target and not other.job.move_dest) {
			workers += 1;
		}
	}
	// the rate of a building (12.5 HP/s) with the factor of units, spec 04 §4.5
	double gain = building::repair_hp(12.5, 1.0, dt, std::max(1, workers), true);
	gain = std::min(gain, static_cast<double>(ts->max_health - *health) - u.job.heal_carry);
	if (gain <= 0.0) {
		return true;
	}
	const auto due = building::repair_due(u.job.repair_owed, u.job.repair_cost, gain, static_cast<double>(ts->max_health));
	resource_amounts_t pay{};
	for (size_t i = 0; i < RESOURCE_COUNT and i < due.size(); ++i) {
		pay[i] = due[i];
	}
	if (state->has_player(*owner) and not state->get_player(*owner)->get_resources().spend(pay)) {
		auto missing = prod::missing_resource(state->get_player(*owner)->get_resources().get(), pay);
		const std::string text = "Reparatur abgebrochen: "
		                         + (missing ? prod::missing_message(*missing) : std::string{"Nicht genug Rohstoffe"});
		if (auto buildings = state->get_buildings()) {
			buildings->post(*owner, text, true);
		}
		return false;
	}
	u.job.heal_carry += gain;
	const auto add = static_cast<int64_t>(std::floor(u.job.heal_carry + 1e-9));
	if (add > 0) {
		u.job.heal_carry -= static_cast<double>(add);
		const int64_t value = std::min(ts->max_health, *health + add);
		live->set_attribute(time, ts->health_attribute, value);
		combat->health_changed(u.job.target, value);
	}
	return true;
}

void SpecialState::heal_step(const std::shared_ptr<GameState> &state, entity_id_t monk, Unit &u, double dt,
                             const time::time_t &time) {
	auto combat = state->get_combat();
	auto m = find_entity(state, monk);
	auto target = find_entity(state, u.job.target);
	auto ts = combat->get_stats(u.job.target);
	auto live = component_of<component::Live>(target, component::component_t::LIVE);
	const auto *mt = this->type_of(state, m, time);
	if (ts == nullptr or live == nullptr or mt == nullptr) {
		return;
	}
	int healers = 0;
	for (const auto &[id, other] : this->units) {
		if (other.job.kind == job_t::HEAL and other.job.target == u.job.target and not other.job.move_dest) {
			healers += 1;
		}
	}
	healers = std::max(1, healers);
	auto owner = owner_of(m, time);
	const double factor = owner ? this->player_resource(state, *owner, 89, 0.0) : 0.0;
	u.job.heal_carry += heal_hp(mt->work_rate, mt->heal_task, dt, healers, factor);
	const auto add = static_cast<int64_t>(std::floor(u.job.heal_carry));
	if (add <= 0) {
		return;
	}
	u.job.heal_carry -= static_cast<double>(add);
	auto health = live->get_attribute(time, ts->health_attribute);
	if (not health) {
		return;
	}
	const int64_t value = std::min(ts->max_health, *health + add);
	live->set_attribute(time, ts->health_attribute, value);
	combat->health_changed(u.job.target, value);
	this->stats.healed += static_cast<double>(value - *health);
}

void SpecialState::auto_heal(const std::shared_ptr<GameState> &state, entity_id_t monk, Unit &u,
                             const time::time_t &time) {
	auto combat = state->get_combat();
	auto m = find_entity(state, monk);
	const auto *mt = this->type_of(state, m, time);
	auto owner = owner_of(m, time);
	if (mt == nullptr or not owner) {
		return;
	}
	const auto here = ground_of(m, time);
	std::optional<entity_id_t> best;
	double best_d = mt->search_range;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (id == monk or combat->is_dead(id) or combat->garrisoned_in(id)) {
			continue;
		}
		auto ts = combat->get_stats(id);
		// herd animals are food: a villager slaughtering a sheep must not fight a healing monk
		if (ts == nullptr or not ts->unit or ts->building or ts->animal or ts->ambient
		    or owner_of(entity, time) != owner) {
			continue;
		}
		const double d = combat::distance(here, ground_of(entity, time));
		if (d >= best_d) {
			continue;
		}
		const auto *tt = this->type_of(state, entity, time);
		if (tt == nullptr or not healable(ts->alive, ts->building, tt->ship, tt->siege)) {
			continue;
		}
		auto live = component_of<component::Live>(entity, component::component_t::LIVE);
		auto health = live ? live->get_attribute(time, ts->health_attribute) : std::nullopt;
		if (not health or *health >= ts->max_health) {
			continue;
		}
		best = id;
		best_d = d;
	}
	if (best) {
		u.job = Job{};
		u.job.kind = job_t::HEAL;
		u.job.target = *best;
		u.job.started = time.to_double();
		log::log(INFO << "Special: " << describe(m, time) << " heals " << *best << " on its own at t="
		              << fmt1(time.to_double()));
	}
}

void SpecialState::relic_gold(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	const auto second = static_cast<int64_t>(std::floor(time.to_double()));
	if (this->gold_second < 0) {
		this->gold_second = second;
		return;
	}
	auto combat = state->get_combat();
	while (this->gold_second < second) {
		this->gold_second += 1;
		for (const auto &[carrier, list] : this->carried) {
			auto entity = find_entity(state, carrier);
			if (entity == nullptr or combat->is_dead(carrier) or not finished(entity)) {
				continue;
			}
			const auto *t = this->type_of(state, entity, time);
			auto owner = owner_of(entity, time);
			if (t == nullptr or not t->monastery or not owner or this->neutral.contains(*owner)
			    or not state->has_player(*owner) or combat->get_match_state(*owner) == combat::match_state_t::DEFEAT) {
				continue;
			}
			size_t relics = 0;
			for (auto id : list) {
				relics += this->type_of(state, find_entity(state, id), time) != nullptr
				                  and this->type_of(state, find_entity(state, id), time)->relic ?
				              1 :
				              0;
			}
			if (relics == 0) {
				continue;
			}
			// 02 §2.10: (n_R + 2 n_X) · R191 / 60 per whole second (n_X = 0 in the base data)
			const double amount = static_cast<double>(relics) * this->player_resource(state, *owner, 191, 30.0) / 60.0;
			state->get_player(*owner)->get_resources().add(resource_t::GOLD, amount);
			this->stats.relic_gold += amount;
		}
	}
}

// ---------------------------------------------------------------- carriers

void SpecialState::stow_into(const std::shared_ptr<GameState> &state, entity_id_t id, entity_id_t carrier,
                             const time::time_t &time) {
	if (not state->get_combat()->stow(state, id, carrier, time)) {
		return;
	}
	this->inside[id] = carrier;
	this->carried[carrier].push_back(id);
}

bool SpecialState::release(const std::shared_ptr<GameState> &state, entity_id_t id, size_t slot, bool require_spot,
                           const time::time_t &time) {
	auto it = this->inside.find(id);
	if (not state->get_combat()->unstow(state, id, slot, require_spot, time)) {
		if (it != this->inside.end() and not require_spot) {
			// no longer in the combat state (removed): forget it
			std::erase(this->carried[it->second], id);
			this->inside.erase(it);
		}
		return false;
	}
	if (it != this->inside.end()) {
		auto c = this->carried.find(it->second);
		if (c != this->carried.end()) {
			std::erase(c->second, id);
			if (c->second.empty()) {
				this->carried.erase(c);
			}
		}
		this->inside.erase(it);
	}
	return true;
}

std::string SpecialState::board_check(const std::shared_ptr<GameState> &state, entity_id_t unit_id,
                                      entity_id_t carrier, const time::time_t &time) {
	auto combat = state->get_combat();
	auto u = find_entity(state, unit_id);
	auto c = find_entity(state, carrier);
	if (u == nullptr or c == nullptr or combat->is_dead(carrier) or combat->is_dead(unit_id) or unit_id == carrier) {
		return "Ziel nicht mehr da";
	}
	if (owner_of(u, time) != owner_of(c, time)) {
		return "nur eigene Transporter";
	}
	const auto *ct = this->type_of(state, c, time);
	const auto *ut = this->type_of(state, u, time);
	auto us = combat->get_stats(unit_id);
	if (ct == nullptr or ut == nullptr or us == nullptr or ct->cargo == cargo_t::NONE) {
		return "nimmt keine Einheiten auf";
	}
	if (static_cast<int>(this->stowed(carrier).size()) >= ct->cargo_slots) {
		return "voll";
	}
	if (not us->unit or us->building or not us->movable or ut->ship or ut->cargo != cargo_t::NONE) {
		return "kann nicht einsteigen";
	}
	if (ct->cargo == cargo_t::RAM and (us->villager or not combat::ram_accepts(us->unit_class))) {
		return "Rammbock nimmt nur Infanterie auf";
	}
	return {};
}

std::vector<entity_id_t> SpecialState::stowed(entity_id_t carrier) const {
	auto it = this->carried.find(carrier);
	return it == this->carried.end() ? std::vector<entity_id_t>{} : it->second;
}

std::optional<entity_id_t> SpecialState::stowed_in(entity_id_t id) const {
	auto it = this->inside.find(id);
	return it == this->inside.end() ? std::nullopt : std::optional<entity_id_t>{it->second};
}

void SpecialState::show_carrying(const std::shared_ptr<GameState> &state, entity_id_t monk, bool carrying,
                                 const time::time_t &time) {
	auto entity = find_entity(state, monk);
	if (entity == nullptr) {
		return;
	}
	std::unordered_map<std::string, std::string> overrides;
	const auto *t = carrying ? this->type_of(state, entity, time) : nullptr;
	if (t != nullptr) {
		const auto idle = ability_animation(entity, component::component_t::IDLE);
		const auto move = ability_animation(entity, component::component_t::MOVE);
		if (not idle.empty() and not t->carry_idle_animation.empty()) {
			overrides[idle] = t->carry_idle_animation;
		}
		if (not move.empty() and not t->carry_move_animation.empty()) {
			overrides[move] = t->carry_move_animation;
		}
	}
	entity->set_animation_overrides(time, overrides);
}

void SpecialState::show_form(const std::shared_ptr<GameState> &state, entity_id_t trebuchet, bool unpacked,
                             const time::time_t &time) {
	auto entity = find_entity(state, trebuchet);
	if (entity == nullptr) {
		return;
	}
	std::unordered_map<std::string, std::string> overrides;
	const auto *t = unpacked ? this->type_of(state, entity, time) : nullptr;
	if (t != nullptr and not t->unpacked_idle_animation.empty()) {
		const auto idle = ability_animation(entity, component::component_t::IDLE);
		if (not idle.empty()) {
			overrides[idle] = t->unpacked_idle_animation;
		}
	}
	if (t != nullptr and not t->unpacked_death_animation.empty()) {
		auto stats = state->get_combat()->get_stats(trebuchet);
		if (stats != nullptr and not stats->death_animation.empty()) {
			overrides[stats->death_animation] = t->unpacked_death_animation;
		}
	}
	entity->set_animation_overrides(time, overrides);
}

std::optional<entity_id_t> SpecialState::carried_relic(entity_id_t monk) const {
	const auto *u = this->find_unit(monk);
	return u ? u->relic : std::nullopt;
}

bool SpecialState::is_unpacked(entity_id_t id) const {
	const auto *u = this->find_unit(id);
	return u and u->unpacked;
}

std::optional<entity_id_t> SpecialState::get_job_target(entity_id_t id) const {
	const auto *u = this->find_unit(id);
	if (u == nullptr or u->job.kind == job_t::NONE or u->job.kind == job_t::UNLOAD) {
		return std::nullopt;
	}
	return u->job.target;
}

std::optional<double> SpecialState::faith(entity_id_t id) const {
	const auto *u = this->find_unit(id);
	if (u == nullptr or not u->has_faith) {
		return std::nullopt;
	}
	return u->faith;
}

// ---------------------------------------------------------------- orders

std::string SpecialState::order_convert(const std::shared_ptr<GameState> &state, entity_id_t monk, entity_id_t target,
                                        const time::time_t &time) {
	auto reason = this->convert_check(state, monk, target, time);
	if (not reason.empty()) {
		return reason;
	}
	auto &u = this->unit(monk);
	if (u.relic) {
		return "trägt eine Reliquie";
	}
	// decision L45: a conversion starts only with full faith
	if (not may_start_conversion(u.faith)) {
		return "Glaube " + std::to_string(static_cast<int>(u.faith)) + "/100 – erst voll aufladen";
	}
	u.job = Job{};
	u.job.kind = job_t::CONVERT;
	u.job.target = target;
	u.job.explicit_order = true;
	u.job.started = time.to_double();
	return {};
}

std::string SpecialState::order_heal(const std::shared_ptr<GameState> &state, entity_id_t monk, entity_id_t target,
                                     const time::time_t &time) {
	auto combat = state->get_combat();
	auto m = find_entity(state, monk);
	auto e = find_entity(state, target);
	const auto *mt = this->type_of(state, m, time);
	const auto *tt = this->type_of(state, e, time);
	auto ts = combat->get_stats(target);
	if (mt == nullptr or not mt->heals) {
		return "kann nicht heilen";
	}
	if (e == nullptr or tt == nullptr or ts == nullptr or combat->is_dead(target) or monk == target) {
		return "kein Ziel";
	}
	if (owner_of(e, time) != owner_of(m, time)) {
		return "nur eigene Einheiten";
	}
	if (ts->animal or ts->ambient) {
		return "nicht heilbar (Tiere)";
	}
	if (not healable(ts->alive, ts->building, tt->ship, tt->siege)) {
		return "nicht heilbar (Gebäude, Schiffe, Belagerung)";
	}
	auto &u = this->unit(monk);
	if (u.relic) {
		return "trägt eine Reliquie";
	}
	u.job = Job{};
	u.job.kind = job_t::HEAL;
	u.job.target = target;
	u.job.explicit_order = true;
	u.job.started = time.to_double();
	return {};
}

std::string SpecialState::order_repair(const std::shared_ptr<GameState> &state, entity_id_t villager, entity_id_t target,
                                       const time::time_t &time) {
	auto combat = state->get_combat();
	auto v = find_entity(state, villager);
	auto e = find_entity(state, target);
	auto ts = combat->get_stats(target);
	const auto *tt = this->type_of(state, e, time);
	if (v == nullptr or not v->has_component(component::component_t::BUILDER)) {
		return "kann nicht reparieren";
	}
	if (e == nullptr or ts == nullptr or tt == nullptr or combat->is_dead(target)) {
		return "kein Ziel";
	}
	if (owner_of(e, time) != owner_of(v, time)) {
		return "nur eigene Einheiten";
	}
	if (not tt->siege and not tt->ship) {
		return "nicht reparierbar (nur Belagerung und Schiffe)";
	}
	auto live = component_of<component::Live>(e, component::component_t::LIVE);
	auto health = live ? live->get_attribute(time, ts->health_attribute) : std::nullopt;
	if (not health or not building::can_repair(static_cast<double>(*health))) {
		return "kein Ziel";
	}
	if (*health >= ts->max_health) {
		return "nicht beschädigt";
	}
	auto &u = this->unit(villager);
	u.job = Job{};
	u.job.kind = job_t::REPAIR;
	u.job.target = target;
	u.job.repair_cost = prod::unit_cost(state, *owner_of(v, time), prod::entity_name(e), time);
	u.job.explicit_order = true;
	u.job.started = time.to_double();
	combat->halt_entity(villager, time);
	log::log(INFO << "Special: " << describe(v, time) << " repairs " << describe(e, time) << " at t="
	              << fmt1(time.to_double()));
	return {};
}

std::string SpecialState::order_pickup(const std::shared_ptr<GameState> &state, entity_id_t monk, entity_id_t relic,
                                       const time::time_t &time) {
	auto m = find_entity(state, monk);
	auto r = find_entity(state, relic);
	const auto *mt = this->type_of(state, m, time);
	const auto *rt = this->type_of(state, r, time);
	if (mt == nullptr or not mt->carries_relics) {
		return "kann keine Reliquie tragen";
	}
	if (rt == nullptr or not rt->relic or this->inside.contains(relic)) {
		return "keine freie Reliquie";
	}
	auto &u = this->unit(monk);
	if (u.relic) {
		return "trägt schon eine Reliquie";
	}
	u.job = Job{};
	u.job.kind = job_t::PICKUP;
	u.job.target = relic;
	u.job.explicit_order = true;
	u.job.started = time.to_double();
	return {};
}

std::string SpecialState::order_deliver(const std::shared_ptr<GameState> &state, entity_id_t monk,
                                        entity_id_t monastery, const time::time_t &time) {
	auto m = find_entity(state, monk);
	auto b = find_entity(state, monastery);
	const auto *bt = this->type_of(state, b, time);
	auto *u = this->find_unit(monk);
	if (u == nullptr or not u->relic) {
		return "trägt keine Reliquie";
	}
	if (bt == nullptr or not bt->monastery or owner_of(b, time) != owner_of(m, time) or not finished(b)) {
		return "nur ins eigene fertige Kloster";
	}
	u->job = Job{};
	u->job.kind = job_t::DELIVER;
	u->job.target = monastery;
	u->job.explicit_order = true;
	u->job.started = time.to_double();
	return {};
}

std::string SpecialState::drop_relic(const std::shared_ptr<GameState> &state, entity_id_t monk,
                                     const time::time_t &time) {
	auto *u = this->find_unit(monk);
	if (u == nullptr or not u->relic) {
		return "trägt keine Reliquie";
	}
	const auto relic = *u->relic;
	this->release(state, relic, 0, false, time);
	u->relic.reset();
	this->show_carrying(state, monk, false, time);
	this->stats.relics_dropped += 1;
	log::log(INFO << "Special: monk " << monk << " drops relic " << relic << " at t=" << fmt1(time.to_double()));
	return {};
}

std::string SpecialState::order_board(const std::shared_ptr<GameState> &state, entity_id_t unit_id,
                                      entity_id_t carrier, const time::time_t &time) {
	auto reason = this->board_check(state, unit_id, carrier, time);
	if (not reason.empty()) {
		return reason;
	}
	// places already promised to units on the way count
	const auto *ct = this->type_of(state, find_entity(state, carrier), time);
	int coming = 0;
	for (const auto &[id, other] : this->units) {
		coming += other.job.kind == job_t::BOARD and other.job.target == carrier and id != unit_id ? 1 : 0;
	}
	if (ct and static_cast<int>(this->stowed(carrier).size()) + coming >= ct->cargo_slots) {
		return "voll";
	}
	auto &u = this->unit(unit_id);
	u.job = Job{};
	u.job.kind = job_t::BOARD;
	u.job.target = carrier;
	u.job.explicit_order = true;
	u.job.started = time.to_double();
	return {};
}

std::string SpecialState::unload(const std::shared_ptr<GameState> &state, entity_id_t carrier,
                                 const std::optional<coord::phys3> &point, const time::time_t &time) {
	auto cargo = this->stowed(carrier);
	auto c = find_entity(state, carrier);
	const auto *ct = this->type_of(state, c, time);
	if (cargo.empty() or ct == nullptr or ct->monastery) {
		return "niemand an Bord";
	}
	if (not point or ct->cargo == cargo_t::RAM) {
		// at once: next to the carrier (a ship needs land next to it)
		size_t out = 0;
		for (size_t i = 0; i < cargo.size(); ++i) {
			out += this->release(state, cargo[i], i, ct->ship, time) ? 1 : 0;
		}
		this->stats.unloaded += out;
		if (out == 0) {
			return "kein Ufer in der Nähe";
		}
		log::log(INFO << "Special: " << describe(c, time) << " unloads " << out << " units at t="
		              << fmt1(time.to_double()));
		return {};
	}
	auto &u = this->unit(carrier);
	u.job = Job{};
	u.job.kind = job_t::UNLOAD;
	u.job.point = point;
	u.job.explicit_order = true;
	u.job.started = time.to_double();
	return {};
}

std::string SpecialState::set_packed(const std::shared_ptr<GameState> &state, entity_id_t trebuchet, bool packed,
                                     const time::time_t &time) {
	auto e = find_entity(state, trebuchet);
	const auto *t = this->type_of(state, e, time);
	if (t == nullptr or not t->packable) {
		return "kann nicht packen";
	}
	auto &u = this->unit(trebuchet);
	if (u.pack_until) {
		return "packt schon";
	}
	if (u.unpacked == not packed) {
		return packed ? "schon abgebaut" : "schon aufgebaut";
	}
	state->get_combat()->halt_entity(trebuchet, time);
	u.pack_target = packed;
	u.pack_until = time.to_double() + PACK_TIME;
	log::log(INFO << "Special: " << describe(e, time) << (packed ? " packs" : " unpacks") << " (" << PACK_TIME
	              << " s) at t=" << fmt1(time.to_double()));
	return {};
}

combat::CombatState::siege_gate_t SpecialState::siege_gate(const std::shared_ptr<GameState> &state, entity_id_t id,
                                                           bool in_range, bool explicit_order, const time::time_t &time) {
	using gate_t = combat::CombatState::siege_gate_t;
	const auto *t = this->type_of(state, find_entity(state, id), time);
	if (t == nullptr or not t->packable) {
		return gate_t::GO;
	}
	auto &u = this->unit(id);
	if (not explicit_order) {
		// on its own a trebuchet only shoots what is in range, and only when it stands unpacked
		return in_range and u.unpacked and not u.pack_until ? gate_t::GO : gate_t::DROP;
	}
	if (u.pack_until) {
		return gate_t::WAIT;
	}
	if (in_range == u.unpacked) {
		return gate_t::GO;
	}
	this->set_packed(state, id, u.unpacked, time);
	return gate_t::WAIT;
}

bool SpecialState::pack_and_move(const std::shared_ptr<GameState> &state, entity_id_t id,
                                 const coord::phys3 &destination, const time::time_t &time) {
	auto *u = this->find_unit(id);
	if (u == nullptr or not(u->unpacked or u->pack_until)) {
		return false;
	}
	if (not u->pack_until) {
		this->set_packed(state, id, true, time);
	}
	else if (not u->pack_target) {
		// unpacking: turn round (the remaining time counts again)
		u->pack_target = true;
		u->pack_until = time.to_double() + PACK_TIME;
	}
	u->after_pack = destination;
	return true;
}

void SpecialState::cancel_job(entity_id_t id) {
	if (auto *u = this->find_unit(id)) {
		u->job = Job{};
		u->after_pack.reset();
		u->last_auto = this->last_tick + 5.0;  // no automatic healing right after a player command
	}
}

// ---------------------------------------------------------------- hooks

bool SpecialState::on_death(const std::shared_ptr<GameState> &state, const std::shared_ptr<GameEntity> &entity,
                            const time::time_t &time) {
	const auto id = entity->get_id();
	auto combat = state->get_combat();
	// a stowed entity died (cargo of a sunken ship)
	if (auto it = this->inside.find(id); it != this->inside.end()) {
		if (auto c = this->carried.find(it->second); c != this->carried.end()) {
			std::erase(c->second, id);
		}
		this->inside.erase(it);
	}
	// a monk carrying a relic: the relic falls (05 §5.9)
	if (auto *u = this->find_unit(id); u and u->relic) {
		const auto relic = *u->relic;
		u->relic.reset();
		this->show_carrying(state, id, false, time);
		this->release(state, relic, 0, false, time);
		this->stats.relics_dropped += 1;
		log::log(INFO << "Special: relic " << relic << " falls from dying " << prod::entity_name(entity) << " " << id);
	}
	auto cargo = this->stowed(id);
	if (not cargo.empty()) {
		const auto *t = this->type_of(state, entity, time);
		const bool sinks = t != nullptr and t->ship;
		size_t slot = 0;
		for (auto inner : cargo) {
			if (sinks and not(this->type_of(state, find_entity(state, inner), time)
			                  and this->type_of(state, find_entity(state, inner), time)->relic)) {
				// the cargo of a ship goes down with it
				this->inside.erase(inner);
				std::erase(this->carried[id], inner);
				this->stats.cargo_lost += 1;
				combat->kill(state, inner, time);
			}
			else {
				this->release(state, inner, slot++, false, time);
			}
		}
		log::log(INFO << "Special: " << prod::entity_name(entity) << " " << id << " died with " << cargo.size()
		              << (sinks ? " units on board (lost)" : " inside (they come out)"));
		this->carried.erase(id);
	}
	this->units.erase(id);
	for (auto &[other_id, other] : this->units) {
		if (other.job.kind != job_t::NONE and other.job.target == id) {
			other.job = Job{};
		}
	}
	return false;
}

void SpecialState::on_impact(const std::shared_ptr<GameState> &state, entity_id_t attacker, player_id_t owner,
                             const combat::CombatStats &stats, std::optional<entity_id_t> main_target, double x0,
                             double y0, const time::time_t &time) {
	auto combat = state->get_combat();
	auto a = find_entity(state, attacker);
	if (a == nullptr) {
		return;
	}
	const auto *at = this->type_of(state, a, time);
	if (at == nullptr or at->blast_range <= 0.0) {
		return;
	}
	const double rho = at->blast_range;
	const bool melee = not stats.ranged;
	splash_t kind = splash_t::PROJECTILE;
	if (melee) {
		if (at->self_destruct) {
			kind = splash_t::DEMOLITION;
		}
		else if (at->siege) {
			kind = splash_t::RAM;
		}
		else if (at->name.find("Elephant") != std::string::npos) {
			kind = splash_t::ELEPHANT;
		}
		else {
			kind = splash_t::FLAT5;
		}
	}
	const bool spare = spares_friendly(melee);
	std::vector<std::pair<entity_id_t, double>> hits;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (id == attacker or (main_target and id == *main_target) or combat->is_dead(id) or combat->garrisoned_in(id)) {
			continue;
		}
		auto ts = combat->get_stats(id);
		if (ts == nullptr or not ts->attackable()) {
			continue;
		}
		auto to = owner_of(entity, time);
		if (not to or this->neutral.contains(*to)) {
			continue;
		}
		if (spare and *to == owner) {
			continue;
		}
		const auto p = ground_of(entity, time);
		if (not in_area(p.x, p.y, ts->radius, ts->radius, x0, y0, rho)) {
			continue;
		}
		const auto *tt = this->type_of(state, entity, time);
		if (tt == nullptr or not area_level_hit(tt->blast_defense, at->blast_offence)) {
			continue;
		}
		const double dx = std::max(0.0, std::abs(p.x - x0) - ts->radius);
		const double dy = std::max(0.0, std::abs(p.y - y0) - ts->radius);
		hits.emplace_back(id, std::hypot(dx, dy));
	}
	std::sort(hits.begin(), hits.end());
	size_t done = 0;
	size_t own = 0;
	for (const auto &[id, d] : hits) {
		auto ts = combat->get_stats(id);
		if (ts == nullptr or not combat->roll_accuracy(stats.accuracy)) {
			continue;
		}
		const double full = static_cast<double>(std::max<int64_t>(1, combat->damage_against(stats, id)));
		const double damage = splash_damage(kind, full, d, rho, false, ts->building);
		if (damage <= 0.0) {
			continue;
		}
		auto entity = find_entity(state, id);
		const bool friendly = entity and owner_of(entity, time) == owner;
		if (combat->area_hit(state, stats, attacker, owner, id, damage / full, time)) {
			done += 1;
			own += friendly ? 1 : 0;
		}
	}
	this->stats.splash_hits += done;
	this->stats.friendly_hits += own;
	if (done > 0) {
		log::log(INFO << "Special: area attack of " << at->name << " " << attacker << " at (" << fmt1(x0) << ", "
		              << fmt1(y0) << ") radius " << fmt1(rho) << " hits " << done << " more" << (own ? " (" + std::to_string(own) + " own)" : std::string{})
		              << " at t=" << fmt1(time.to_double()));
	}
	if (at->self_destruct and not combat->is_dead(attacker)) {
		this->stats.self_destructs += 1;
		combat->kill(state, attacker, time);
	}
}

// ---------------------------------------------------------------- snapshot, save games

void SpecialState::publish(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	std::unordered_map<entity_id_t, SpecialInfo> next;
	auto job_text = [](job_t kind) -> std::string {
		switch (kind) {
		case job_t::CONVERT:
			return "Konvertiert";
		case job_t::HEAL:
			return "Heilt";
		case job_t::PICKUP:
			return "Holt Reliquie";
		case job_t::DELIVER:
			return "Bringt Reliquie";
		case job_t::BOARD:
			return "Steigt ein";
		case job_t::UNLOAD:
			return "Fährt zum Entladen";
		default:
			return {};
		}
	};
	for (const auto &[id, u] : this->units) {
		auto entity = find_entity(state, id);
		const auto *t = this->type_of(state, entity, time);
		SpecialInfo info;
		info.monk = t and t->monk;
		info.heals = t and t->heals;
		info.faith = u.has_faith ? static_cast<int>(std::floor(u.faith + 1e-6)) : 0;
		info.job = job_text(u.job.kind);
		info.relic = u.relic.has_value();
		info.packable = t and t->packable;
		info.unpacked = u.unpacked;
		info.packing = u.pack_until.has_value();
		if (info.packing) {
			info.job = u.pack_target ? "Baut ab" : "Baut auf";
		}
		next[id] = info;
	}
	for (const auto &[id, entity] : state->get_game_entities()) {
		(void)entity;
		auto it = this->carried.find(id);
		const auto *t = this->type_of(state, find_entity(state, id), time);
		if (t == nullptr or (it == this->carried.end() and t->cargo == cargo_t::NONE and not t->monastery)) {
			continue;
		}
		auto &info = next[id];
		if (t->monastery) {
			info.relics = it == this->carried.end() ? 0 : static_cast<int>(it->second.size());
			if (info.relics > 0) {
				if (auto owner = owner_of(entity, time)) {
					info.relic_gold = info.relics * this->player_resource(state, *owner, 191, 30.0) / 60.0;
				}
			}
		}
		else {
			info.cargo = it == this->carried.end() ? 0 : static_cast<int>(it->second.size());
			info.cargo_max = t->cargo_slots;
			info.transport = t->cargo == cargo_t::TRANSPORT;
		}
	}
	std::lock_guard<std::mutex> lock{this->snapshot_mutex};
	this->snapshot = std::move(next);
}

std::optional<SpecialInfo> SpecialState::get_info(entity_id_t id) const {
	std::lock_guard<std::mutex> lock{this->snapshot_mutex};
	auto it = this->snapshot.find(id);
	return it == this->snapshot.end() ? std::nullopt : std::optional<SpecialInfo>{it->second};
}

std::optional<SavedSpecial> SpecialState::save_entity(entity_id_t id) const {
	SavedSpecial s;
	bool any = false;
	if (const auto *u = this->find_unit(id)) {
		if (u->has_faith and u->faith < 100.0 - 1e-9) {
			s.faith = u->faith;
			any = true;
		}
		if (u->job.kind == job_t::CONVERT) {
			s.convert_target = u->job.target;
			s.convert_time = u->job.prayer_since ? this->last_tick - *u->job.prayer_since : 0.0;
			any = true;
		}
		else if (u->job.kind == job_t::HEAL and u->job.explicit_order) {
			s.heal_target = u->job.target;
			any = true;
		}
		if (u->relic) {
			s.relic_carry = *u->relic;
			any = true;
		}
		if (u->unpacked) {
			s.siege_mode = "unpacked";
			any = true;
		}
	}
	if (auto it = this->carried.find(id); it != this->carried.end() and not it->second.empty()) {
		for (auto inner : it->second) {
			if (this->find_unit(id) and this->find_unit(id)->relic == inner) {
				continue;
			}
			(this->relic_ids.contains(inner) ? s.relic_store : s.cargo).push_back(inner);
		}
		any = any or not s.cargo.empty() or not s.relic_store.empty();
	}
	return any ? std::optional<SavedSpecial>{s} : std::nullopt;
}

void SpecialState::load_entity(const std::shared_ptr<GameState> &state, entity_id_t id, const SavedSpecial &saved,
                               const std::function<entity_id_t(entity_id_t)> &mapped, const time::time_t &time) {
	auto entity = find_entity(state, id);
	if (entity == nullptr) {
		return;
	}
	this->seen.insert(id);
	const auto *t = this->type_of(state, entity, time);
	auto &u = this->unit(id);
	u.has_faith = t and (t->monk or t->faith_rate > 0.0);
	if (saved.faith) {
		u.faith = std::clamp(*saved.faith, 0.0, 100.0);
	}
	if (saved.convert_target) {
		u.job = Job{};
		u.job.kind = job_t::CONVERT;
		u.job.target = mapped(*saved.convert_target);
		u.job.explicit_order = true;
		u.job.prayer_since = time.to_double() - saved.convert_time;
		u.job.next_attempt = time.to_double() + (t ? t->convert_reload : 1.0);
	}
	else if (saved.heal_target) {
		u.job = Job{};
		u.job.kind = job_t::HEAL;
		u.job.target = mapped(*saved.heal_target);
		u.job.explicit_order = true;
	}
	if (saved.relic_carry) {
		const auto relic = mapped(*saved.relic_carry);
		this->stow_into(state, relic, id, time);
		if (this->inside.contains(relic)) {
			u.relic = relic;
			this->show_carrying(state, id, true, time);
		}
	}
	for (auto inner : saved.relic_store) {
		this->stow_into(state, mapped(inner), id, time);
	}
	for (auto inner : saved.cargo) {
		this->stow_into(state, mapped(inner), id, time);
	}
	if (saved.siege_mode == "unpacked" and t and t->packable) {
		u.unpacked = true;
		this->show_form(state, id, true, time);
	}
	if (u.job.kind == job_t::NONE and not u.relic and not u.unpacked and not u.has_faith) {
		this->units.erase(id);
	}
}

SpecialState::Counters SpecialState::counters() const {
	return this->stats;
}

// ---------------------------------------------------------------- commands

namespace {

PickRequest make_request(const coord::phys3 &target, const openage::event::EventHandler::param_map &params) {
	PickRequest request;
	request.terrain = target;
	if (params.contains("camera_matrix") and params.contains("pick_ndc")) {
		request.camera = params.get<Eigen::Matrix4f>("camera_matrix", Eigen::Matrix4f::Identity());
		request.ndc = params.get<Eigen::Vector2f>("pick_ndc", Eigen::Vector2f{0.0f, 0.0f});
		request.slack = 0.01;
	}
	return request;
}

/// the entity of the command: "target_entity" or the one under the cursor that \p accept takes
std::shared_ptr<GameEntity> pick(const std::shared_ptr<GameState> &state,
                                 const time::time_t &time,
                                 const coord::phys3 &target,
                                 const openage::event::EventHandler::param_map &params,
                                 const std::function<bool(const std::shared_ptr<GameEntity> &)> &accept) {
	auto combat = state->get_combat();
	if (params.contains("target_entity")) {
		auto e = find_entity(state, params.get<entity_id_t>("target_entity", 0));
		return e != nullptr and accept(e) ? e : nullptr;
	}
	return pick_entity(state, time, make_request(target, params),
	                   [&](const std::shared_ptr<GameEntity> &entity) -> std::optional<PickShape> {
						   if (combat->is_dead(entity->get_id()) or combat->garrisoned_in(entity->get_id())
						       or not accept(entity)) {
							   return std::nullopt;
						   }
						   auto stats = combat->get_stats(entity->get_id());
						   if (stats == nullptr) {
							   return std::nullopt;
						   }
						   return PickShape{stats->radius, stats->building ? stats->radius : 1.0};
					   });
}

} // namespace

bool repair_command(const std::shared_ptr<GameState> &state,
                    const time::time_t &time,
                    const std::vector<entity_id_t> &ids,
                    const coord::phys3 &target,
                    const openage::event::EventHandler::param_map &params) {
	auto special = state->get_special();
	auto combat = state->get_combat();
	if (special == nullptr or combat == nullptr) {
		return false;
	}
	std::vector<entity_id_t> villagers;
	for (auto id : ids) {
		auto e = find_entity(state, id);
		if (e != nullptr and e->has_component(component::component_t::BUILDER)) {
			villagers.push_back(id);
		}
	}
	if (villagers.empty()) {
		return false;
	}
	const auto owner = owner_of(state->get_game_entity(villagers.front()), time);
	auto entity = pick(state, time, target, params, [&](const std::shared_ptr<GameEntity> &e) {
		const auto *t = special->type_of(state, e, time);
		auto stats = combat->get_stats(e->get_id());
		auto live = component_of<component::Live>(e, component::component_t::LIVE);
		if (t == nullptr or stats == nullptr or live == nullptr or owner_of(e, time) != owner or not(t->siege or t->ship)) {
			return false;
		}
		auto health = live->get_attribute(time, stats->health_attribute);
		return health and *health < stats->max_health;
	});
	if (entity == nullptr) {
		return false;
	}
	size_t done = 0;
	for (auto id : villagers) {
		done += special->order_repair(state, id, entity->get_id(), time).empty() ? 1 : 0;
	}
	log::log(INFO << "Special command repair of " << entity->get_id() << ": " << done << " of " << villagers.size()
	              << " villagers");
	return done > 0;
}

bool handle_command(const std::shared_ptr<GameState> &state,
                    const time::time_t &time,
                    component::command::command_t type,
                    std::vector<entity_id_t> &ids,
                    const coord::phys3 &target,
                    const openage::event::EventHandler::param_map &params) {
	using component::command::command_t;
	auto special = state->get_special();
	auto combat = state->get_combat();
	if (special == nullptr or not special->is_started() or combat == nullptr) {
		return false;
	}
	std::erase_if(ids, [&](entity_id_t id) {
		return not state->get_game_entities().contains(id) or combat->is_dead(id) or combat->garrisoned_in(id);
	});
	if (ids.empty()) {
		return false;
	}
	const bool plain = params.get<bool>("plain", false);
	const int arg = params.get<int>("combat_arg", 0);
	// a new command ends the special job (stances, formations and the bell do not)
	if (type != command_t::STANCE and type != command_t::FORMATION and type != command_t::BELL) {
		for (auto id : ids) {
			special->cancel_job(id);
		}
	}
	auto owner = owner_of(state->get_game_entity(ids.front()), time);
	auto type_of = [&](entity_id_t id) {
		return special->type_of(state, state->get_game_entity(id), time);
	};
	size_t done = 0;
	std::string why;
	auto note = [&](const std::string &reason) {
		if (reason.empty()) {
			done += 1;
		}
		else if (why.empty()) {
			why = reason;
		}
	};
	auto report = [&](const char *what) {
		log::log(INFO << "Special command " << what << ": " << done << " of " << ids.size() << " entities"
		              << (why.empty() ? std::string{} : " (" + why + ")"));
	};

	switch (type) {
	case command_t::CONVERT:
	case command_t::HEAL: {
		const bool convert = type == command_t::CONVERT;
		auto entity = pick(state, time, target, params, [&](const std::shared_ptr<GameEntity> &e) {
			auto o = owner_of(e, time);
			auto stats = combat->get_stats(e->get_id());
			return o and stats and stats->attackable() and (convert ? *o != *owner : *o == *owner);
		});
		if (entity == nullptr) {
			why = convert ? "kein gegnerisches Ziel" : "keine eigene Einheit";
		}
		else {
			for (auto id : ids) {
				note(convert ? special->order_convert(state, id, entity->get_id(), time) :
				               special->order_heal(state, id, entity->get_id(), time));
			}
		}
		report(convert ? "convert" : "heal");
		ids.clear();
		return true;
	}
	case command_t::PACK:
	case command_t::UNPACK:
		for (auto id : ids) {
			if (type == command_t::PACK) {
				// packing by hand ends an attack, it would unpack again
				state->get_combat()->stop(state, id, time);
			}
			note(special->set_packed(state, id, type == command_t::PACK, time));
		}
		report(type == command_t::PACK ? "pack" : "unpack");
		ids.clear();
		return true;
	case command_t::DROP_RELIC:
		for (auto id : ids) {
			note(special->drop_relic(state, id, time));
		}
		report("drop relic");
		ids.clear();
		return true;
	case command_t::UNLOAD:
		for (auto id : ids) {
			note(special->unload(state, id, arg != 0 ? std::nullopt : std::optional<coord::phys3>{target}, time));
		}
		report("unload");
		ids.clear();
		return true;
	case command_t::UNGARRISON: {
		// carriers with cargo let it out here (buildings: the W4 garrison)
		std::erase_if(ids, [&](entity_id_t id) {
			if (special->stowed(id).empty()) {
				return false;
			}
			const auto *t = type_of(id);
			if (t == nullptr or t->monastery) {
				return false;
			}
			note(special->unload(state, id, std::nullopt, time));
			return true;
		});
		if (done > 0 or not why.empty()) {
			report("unload");
		}
		return ids.empty();
	}
	case command_t::GARRISON: {
		auto carrier = pick(state, time, target, params, [&](const std::shared_ptr<GameEntity> &e) {
			const auto *t = special->type_of(state, e, time);
			return t and t->cargo != cargo_t::NONE and owner_of(e, time) == owner;
		});
		if (carrier == nullptr) {
			return false;
		}
		std::erase_if(ids, [&](entity_id_t id) {
			auto r = special->order_board(state, id, carrier->get_id(), time);
			note(r);
			return r.empty();
		});
		report("board");
		ids.clear();
		return true;
	}
	case command_t::MOVE:
	case command_t::ATTACK: {
		if (plain) {
			// walking on: unpacked trebuchets pack first
			std::erase_if(ids, [&](entity_id_t id) {
				return special->pack_and_move(state, id, target, time);
			});
			return false;
		}
		// --- monks: enemy = convert, relic = pick up, own monastery = deliver, own wounded unit = heal
		std::vector<entity_id_t> monks;
		for (auto id : ids) {
			const auto *t = type_of(id);
			if (t and (t->monk or t->carries_relics)) {
				monks.push_back(id);
			}
		}
		if (not monks.empty()) {
			auto entity = pick(state, time, target, params, [&](const std::shared_ptr<GameEntity> &e) {
				const auto *t = special->type_of(state, e, time);
				auto stats = combat->get_stats(e->get_id());
				return t and stats and (t->relic or (stats->unit or stats->building));
			});
			const auto *et = entity ? special->type_of(state, entity, time) : nullptr;
			auto eo = entity ? owner_of(entity, time) : std::nullopt;
			std::string action;
			if (entity and et) {
				if (et->relic) {
					action = "pick up relic";
					bool given = false;
					for (auto id : monks) {
						if (not given and not special->carried_relic(id)) {
							auto r = special->order_pickup(state, id, entity->get_id(), time);
							note(r);
							given = r.empty();
						}
					}
				}
				else if (et->monastery and eo == owner) {
					action = "deliver relic";
					for (auto id : monks) {
						if (special->carried_relic(id)) {
							note(special->order_deliver(state, id, entity->get_id(), time));
						}
					}
				}
				else if (eo and owner and *eo != *owner and not special->neutral_player(*eo)) {
					action = "convert";
					for (auto id : monks) {
						note(special->order_convert(state, id, entity->get_id(), time));
					}
				}
				else if (eo == owner and entity->get_id() != monks.front()) {
					action = "heal";
					for (auto id : monks) {
						note(special->order_heal(state, id, entity->get_id(), time));
					}
				}
			}
			if (not action.empty()) {
				const size_t ordered = done;
				// ordered monks keep their job, the others walk
				std::erase_if(ids, [&](entity_id_t id) {
					return std::find(monks.begin(), monks.end(), id) != monks.end()
					       and special->get_job_target(id).has_value();
				});
				log::log(INFO << "Special command (right click) " << action << ": " << ordered << " of " << monks.size()
				              << " monks" << (why.empty() ? std::string{} : " (" + why + ")"));
				done = 0;
				why.clear();
			}
		}
		// --- land units onto an own transport ship or ram
		auto carrier = pick(state, time, target, params, [&](const std::shared_ptr<GameEntity> &e) {
			const auto *t = special->type_of(state, e, time);
			return t and t->cargo != cargo_t::NONE and owner_of(e, time) == owner;
		});
		if (carrier != nullptr) {
			std::erase_if(ids, [&](entity_id_t id) {
				if (id == carrier->get_id()) {
					return false;
				}
				auto r = special->order_board(state, id, carrier->get_id(), time);
				note(r);
				return r.empty();
			});
			if (done > 0 or not why.empty()) {
				report("board (right click)");
			}
			done = 0;
			why.clear();
		}
		// --- loaded transport ships: a point on land unloads there
		std::erase_if(ids, [&](entity_id_t id) {
			const auto *t = type_of(id);
			if (t == nullptr or t->cargo != cargo_t::TRANSPORT or special->stowed(id).empty()) {
				return false;
			}
			auto grid = grid_of(state, state->get_game_entity(id));
			if (not grid or state->get_map()->is_passable(*grid, target.to_tile())) {
				return false;  // water: the ship sails there
			}
			auto r = special->unload(state, id, target, time);
			note(r);
			return r.empty();
		});
		if (done > 0 or not why.empty()) {
			report("unload at the shore (right click)");
		}
		// --- unpacked trebuchets pack before they walk; an enemy under the click is an attack (the combat
		// rule decides whether it has to pack for it)
		const bool on_enemy = pick(state, time, target, params, [&](const std::shared_ptr<GameEntity> &e) {
			const auto eo = owner_of(e, time);
			return eo and owner and *eo != *owner and not special->neutral_player(*eo);
		}) != nullptr;
		std::erase_if(ids, [&](entity_id_t id) {
			return type == command_t::MOVE and not on_enemy and special->pack_and_move(state, id, target, time);
		});
		return false;
	}
	default:
		return false;
	}
}

// ---------------------------------------------------------------- event handler

SpecialEventHandler::SpecialEventHandler() :
	OnceEventHandler{"game.special_tick"} {
}

void SpecialEventHandler::setup_event(const std::shared_ptr<openage::event::Event> & /* event */,
                                      const std::shared_ptr<openage::event::State> & /* state */) {
}

void SpecialEventHandler::invoke(openage::event::EventLoop & /* loop */,
                                 const std::shared_ptr<openage::event::EventEntity> &target,
                                 const std::shared_ptr<openage::event::State> &state,
                                 const time::time_t &time,
                                 const param_map & /* params */) {
	auto special = std::dynamic_pointer_cast<SpecialState>(target);
	auto gstate = std::dynamic_pointer_cast<GameState>(state);
	if (special and gstate) {
		special->tick(gstate, time);
	}
}

time::time_t SpecialEventHandler::predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> & /* target */,
                                                      const std::shared_ptr<openage::event::State> & /* state */,
                                                      const time::time_t &at) {
	return at;
}

} // namespace openage::gamestate::special
