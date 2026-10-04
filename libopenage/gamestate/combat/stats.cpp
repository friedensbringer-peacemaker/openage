// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "stats.h"

#include <exception>
#include <vector>

#include "log/log.h"
#include "log/message.h"

#include "gamestate/api/ability.h"
#include "gamestate/api/animation.h"
#include "gamestate/api/property.h"
#include "gamestate/api/types.h"


namespace openage::gamestate::combat {

namespace {

constexpr const char *FLAC_EFFECT = "engine.effect.discrete.flat_attribute_change.type.FlatAttributeChangeDecrease";
constexpr const char *FLAC_RESISTANCE = "engine.resistance.discrete.flat_attribute_change.type.FlatAttributeChangeDecrease";

std::string short_name(const nyan::fqon_t &fqon) {
	auto pos = fqon.rfind('.');
	return pos == std::string::npos ? fqon : fqon.substr(pos + 1);
}

/// float or int member (the converter writes some ranges as int)
double number(const nyan::Object &obj, const std::string &member, double fallback) {
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

std::vector<nyan::Object> objects(const std::shared_ptr<nyan::View> &db_view, const nyan::set_t &set) {
	std::vector<nyan::Object> result;
	for (const auto &value : set) {
		auto obj_value = std::dynamic_pointer_cast<nyan::ObjectValue>(value.get_ptr());
		if (obj_value) {
			result.push_back(db_view->get_object(obj_value->get_name()));
		}
	}
	return result;
}

std::string first_animation(const nyan::Object &ability) {
	if (not api::APIAbility::check_property(ability, api::ability_property_t::ANIMATED)) {
		return {};
	}
	auto property = api::APIAbility::get_property(ability, api::ability_property_t::ANIMATED);
	auto paths = api::APIAnimation::get_animation_paths(api::APIAbilityProperty::get_animations(property));
	return paths.empty() ? std::string{} : paths[0];
}

/// effects of the batches of an ApplyDiscreteEffect ability
AttackProfile read_batches(const std::shared_ptr<nyan::View> &db_view, const nyan::Object &ability) {
	AttackProfile attack;
	for (const auto &batch : objects(db_view, ability.get_set("ApplyDiscreteEffect.batches"))) {
		for (const auto &effect : objects(db_view, batch.get_set("EffectBatch.effects"))) {
			if (not effect.extends(FLAC_EFFECT)) {
				continue;
			}
			Effect e;
			e.type = effect.get_object("FlatAttributeChange.type").get_name();
			e.amount = effect.get_object("FlatAttributeChange.change_value").get_int("AttributeAmount.amount");
			if (effect.has_member("FlatAttributeChange.min_change_value")) {
				e.min_amount = effect.get_object("FlatAttributeChange.min_change_value").get_int("AttributeAmount.amount");
			}
			attack.effects.push_back(e);
		}
	}
	return attack;
}

void read_types(CombatStats &stats, const std::shared_ptr<nyan::View> &db_view, const nyan::Object &entity) {
	for (const auto &type : objects(db_view, entity.get_set("GameEntity.types"))) {
		auto name = short_name(type.get_name());
		if (name == "Unit") {
			stats.unit = true;
		}
		else if (name == "Building") {
			stats.building = true;
		}
		else if (name == "Villager") {
			stats.villager = true;
		}
		else if (name == "Ambient") {
			stats.ambient = true;
		}
		else if (name == "Herdable" or name == "AnimalPrey") {
			stats.herdable = true;
		}
	}
}

void read_ability(CombatStats &stats, const std::shared_ptr<nyan::View> &db_view, const nyan::Object &ability) {
	const auto &parent = ability.get_parents()[0];
	if (parent == "engine.ability.type.Live") {
		for (const auto &setting : objects(db_view, ability.get_set("Live.attributes"))) {
			auto attribute = setting.get_object("AttributeSetting.attribute").get_name();
			if (short_name(attribute) != "Health" and not stats.health_attribute.empty()) {
				continue;
			}
			stats.alive = true;
			stats.health_attribute = attribute;
			stats.max_health = setting.get_int("AttributeSetting.max_value");
			stats.start_health = setting.get_int("AttributeSetting.starting_value");
		}
	}
	else if (parent == "engine.ability.type.ApplyDiscreteEffect" and not stats.can_attack) {
		// melee (the villager's Attack; Construct/Repair are continuous effects)
		stats.attack = read_batches(db_view, ability);
		stats.can_attack = not stats.attack.effects.empty();
		stats.ranged = false;
		stats.reload_time = number(ability, "ApplyDiscreteEffect.reload_time", stats.reload_time);
		stats.attack_animation = first_animation(ability);
	}
	else if (parent == "engine.ability.type.ShootProjectile") {
		// buildings shoot only with min_projectiles > 0 (AoE II: town centers need a garrison)
		auto min_projectiles = static_cast<int64_t>(number(ability, "ShootProjectile.min_projectiles", 1));
		auto projectiles = ability.get_orderedset("ShootProjectile.projectiles");
		if (min_projectiles <= 0 or projectiles.size() == 0) {
			return;
		}
		auto projectile_value = std::dynamic_pointer_cast<nyan::ObjectValue>((*projectiles.begin()).get_ptr());
		if (not projectile_value) {
			return;
		}
		auto projectile = db_view->get_object(projectile_value->get_name());
		for (const auto &p_ability : objects(db_view, projectile.get_set("GameEntity.abilities"))) {
			if (p_ability.get_parents()[0] == "engine.ability.type.ApplyDiscreteEffect") {
				stats.attack = read_batches(db_view, p_ability);
				break;
			}
		}
		stats.can_attack = not stats.attack.effects.empty();
		stats.ranged = true;
		stats.reload_time = number(ability, "ShootProjectile.reload_time", stats.reload_time);
		stats.min_range = number(ability, "ShootProjectile.min_range", 0.0);
		stats.max_range = number(ability, "ShootProjectile.max_range", 4.0);
		stats.attack_animation = first_animation(ability);
	}
	else if (parent == "engine.ability.type.Resistance") {
		for (const auto &resistance : objects(db_view, ability.get_set("Resistance.resistances"))) {
			if (not resistance.extends(FLAC_RESISTANCE)) {
				continue;
			}
			auto type = resistance.get_object("FlatAttributeChange.type").get_name();
			auto block = resistance.get_object("FlatAttributeChange.block_value").get_int("AttributeAmount.amount");
			stats.armor.block[type] = block;
		}
	}
	else if (parent == "engine.ability.type.LineOfSight") {
		stats.line_of_sight = number(ability, "LineOfSight.range", stats.line_of_sight);
	}
	else if (parent == "engine.ability.type.Collision") {
		auto hitbox = ability.get_object("Collision.hitbox");
		stats.radius = std::max(number(hitbox, "Hitbox.radius_x", 0.25), number(hitbox, "Hitbox.radius_y", 0.25));
		stats.height = number(hitbox, "Hitbox.radius_z", 1.0);
	}
	else if (parent == "engine.ability.type.PassiveTransformTo") {
		// "Death" (health 0 -> dead state)
		stats.death_time = number(ability, "PassiveTransformTo.transform_time", 0.0);
		stats.death_animation = first_animation(ability);
	}
	else if (parent == "engine.ability.type.Move") {
		stats.movable = true;
	}
}

} // namespace


std::shared_ptr<const CombatStats> read_combat_stats(const std::shared_ptr<nyan::View> &db_view,
                                                     const nyan::fqon_t &entity_fqon) {
	auto stats = std::make_shared<CombatStats>();
	stats->name = short_name(entity_fqon);
	try {
		auto entity = db_view->get_object(entity_fqon);
		read_types(*stats, db_view, entity);
		for (const auto &ability : objects(db_view, entity.get_set("GameEntity.abilities"))) {
			try {
				read_ability(*stats, db_view, ability);
			}
			catch (std::exception &err) {
				log::log(WARN << "Combat: cannot read " << ability.get_name() << ": " << err.what());
			}
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Combat: cannot read " << entity_fqon << ": " << err.what());
	}
	if (stats->death_time > 10.0) {
		stats->death_time = 10.0;
	}
	return stats;
}

} // namespace openage::gamestate::combat
