// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "stats.h"

#include <cmath>
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
constexpr const char *GENIE_UNIT = "engine.xr.ability.GenieUnit";
constexpr const char *ELEVATION_HIGH_MODIFIER = "engine.modifier.effect.flat_attribute_change.type.ElevationDifferenceHigh";

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

/// int member, fallback if missing (engine.xr fields are ints)
int64_t integer(const nyan::Object &obj, const std::string &member, int64_t fallback) {
	try {
		return obj.get_int(member);
	}
	catch (std::exception &) {
	}
	try {
		return static_cast<int64_t>(std::llround(obj.get_float(member)));
	}
	catch (std::exception &) {
	}
	return fallback;
}

template <typename S>
std::vector<nyan::Object> objects(const std::shared_ptr<nyan::View> &db_view, const S &set) {
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

/// multiplier of an elevation modifier "from above" (05 §5.2), nothing if \p modifier is none
std::optional<double> elevation_multiplier(const std::shared_ptr<nyan::View> &db_view, const nyan::Object &modifier) {
	if (not modifier.extends(ELEVATION_HIGH_MODIFIER)) {
		return std::nullopt;
	}
	try {
		auto mult = db_view->get_object(modifier.get_name() + ".Multiplier");
		return number(mult, "Multiplier.multiplier", 1.25);
	}
	catch (std::exception &) {
	}
	return 1.25;
}

void read_modifiers(CombatStats &stats, const std::shared_ptr<nyan::View> &db_view, const nyan::Object &entity) {
	try {
		for (const auto &modifier : objects(db_view, entity.get_set("GameEntity.modifiers"))) {
			if (auto m = elevation_multiplier(db_view, modifier)) {
				stats.elevation_high = *m;
			}
		}
	}
	catch (std::exception &) {
	}
}

/// flight values of a projectile game entity (XR fork W4)
ProjectileStats read_projectile(CombatStats &stats, const std::shared_ptr<nyan::View> &db_view, const nyan::Object &projectile) {
	ProjectileStats p;
	read_modifiers(stats, db_view, projectile);
	for (const auto &ability : objects(db_view, projectile.get_set("GameEntity.abilities"))) {
		const auto &parent = ability.get_parents()[0];
		try {
			if (parent == "engine.ability.type.ApplyDiscreteEffect" and p.attack.effects.empty()) {
				p.attack = read_batches(db_view, ability);
			}
			else if (parent == "engine.ability.type.Move") {
				p.speed = number(ability, "Move.speed", p.speed);
				if (not std::isfinite(p.speed)) {
					p.speed = 0.0;
				}
			}
			else if (parent == "engine.ability.type.Projectile") {
				p.arc = number(ability, "Projectile.arc", 0.0);
				for (const auto &acc : objects(db_view, ability.get_set("Projectile.accuracy"))) {
					p.accuracy = number(acc, "Accuracy.accuracy", p.accuracy);
					p.dispersion = number(acc, "Accuracy.accuracy_dispersion", p.dispersion);
					break;
				}
				p.lead = short_name(ability.get_object("Projectile.target_mode").get_name()) == "ExpectedPosition";
			}
			else if (parent == GENIE_UNIT) {
				p.pierce = integer(ability, "GenieUnitData.penetration_mode", 0) != 0;
			}
		}
		catch (std::exception &err) {
			log::log(DBG << "Combat: projectile " << projectile.get_name() << ": " << err.what());
		}
	}
	return p;
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

/// values of the data (engine.xr GenieUnitData, W0) for round W4
struct GenieValues {
	bool present = false;
	int unit_class = -1;
	int garrison_capacity = 0;
	int garrison_mask = 0;
	double garrison_heal = 0.0;
	int min_projectiles = 0;
	int max_projectiles = 0;
	double accuracy = 100.0;
	int defense_terrain = -1;
};

void read_genie(GenieValues &g, const nyan::Object &data, bool head) {
	if (head) {
		g.unit_class = static_cast<int>(integer(data, "GenieUnitData.unit_class", -1));
		g.accuracy = number(data, "GenieUnitData.accuracy", 100.0);
		g.defense_terrain = static_cast<int>(integer(data, "GenieUnitData.defense_terrain_bonus", -1));
	}
	// garrison and arrows: the largest values of the head unit and its variants (the town
	// center keeps them in a variant of the data, docs/KONVERTER-FELDER.md)
	auto capacity = static_cast<int>(integer(data, "GenieUnitData.garrison_capacity", 0));
	if (capacity > g.garrison_capacity) {
		g.garrison_capacity = capacity;
		g.garrison_mask = static_cast<int>(integer(data, "GenieUnitData.garrison_type", 0));
		g.garrison_heal = number(data, "GenieUnitData.garrison_heal_rate", 0.0);
	}
	g.min_projectiles = std::max(g.min_projectiles, static_cast<int>(integer(data, "GenieUnitData.projectile_min_count", 0)));
	g.max_projectiles = std::max(g.max_projectiles, static_cast<int>(integer(data, "GenieUnitData.projectile_max_count", 0)));
}

/// raw ShootProjectile values, finished after all abilities are read
struct ShootValues {
	bool present = false;
	int min_projectiles = 0;
	int max_projectiles = 0;
};

void read_ability(CombatStats &stats,
                  GenieValues &genie,
                  ShootValues &shoot,
                  const std::shared_ptr<nyan::View> &db_view,
                  const nyan::Object &ability) {
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
		auto projectiles = objects(db_view, ability.get_orderedset("ShootProjectile.projectiles"));
		if (projectiles.empty()) {
			return;
		}
		shoot.present = true;
		shoot.min_projectiles = static_cast<int>(number(ability, "ShootProjectile.min_projectiles", 1));
		shoot.max_projectiles = static_cast<int>(number(ability, "ShootProjectile.max_projectiles", 1));
		stats.projectile = read_projectile(stats, db_view, projectiles[0]);
		if (projectiles.size() > 1) {
			stats.secondary = read_projectile(stats, db_view, projectiles[1]);
			stats.has_secondary = true;
		}
		stats.attack = stats.projectile.attack;
		stats.can_attack = not stats.attack.effects.empty();
		stats.ranged = true;
		stats.reload_time = number(ability, "ShootProjectile.reload_time", stats.reload_time);
		stats.min_range = number(ability, "ShootProjectile.min_range", 0.0);
		stats.max_range = number(ability, "ShootProjectile.max_range", 4.0);
		stats.spawn_width = number(ability, "ShootProjectile.spawning_area_width", 0.0);
		stats.spawn_length = number(ability, "ShootProjectile.spawning_area_height", 0.0);
		stats.spawn_height = number(ability, "ShootProjectile.spawning_area_offset_z", 1.0);
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
		stats.speed = number(ability, "Move.speed", 0.0);
		if (not std::isfinite(stats.speed)) {
			stats.speed = 0.0;
		}
	}
	else if (parent == "engine.ability.type.GameEntityStance") {
		stats.stances = true;
	}
	else if (parent == "engine.ability.type.Formation") {
		stats.formations = true;
		for (const auto &f : objects(db_view, ability.get_set("Formation.formations"))) {
			auto sub = f.get_object("GameEntityFormation.subformation");
			stats.formation_priority = static_cast<int>(integer(sub, "Subformation.ordering_priority", 0));
			break;
		}
	}
	else if (parent == "engine.ability.type.Storage") {
		// garrison places without engine.xr data (older modpacks)
		auto container = ability.get_object("Storage.container");
		stats.garrison_capacity = std::max(stats.garrison_capacity,
		                                   static_cast<int>(integer(container, "EntityContainer.slots", 0)));
	}
	else if (parent == GENIE_UNIT) {
		genie.present = true;
		read_genie(genie, ability, true);
		for (const auto &variant : objects(db_view, ability.get_orderedset("GenieUnit.variants"))) {
			read_genie(genie, variant, false);
		}
	}
}

} // namespace


std::shared_ptr<const CombatStats> read_combat_stats(const std::shared_ptr<nyan::View> &db_view,
                                                     const nyan::fqon_t &entity_fqon) {
	auto stats = std::make_shared<CombatStats>();
	stats->name = short_name(entity_fqon);
	GenieValues genie;
	ShootValues shoot;
	try {
		auto entity = db_view->get_object(entity_fqon);
		read_types(*stats, db_view, entity);
		read_modifiers(*stats, db_view, entity);
		for (const auto &ability : objects(db_view, entity.get_set("GameEntity.abilities"))) {
			try {
				read_ability(*stats, genie, shoot, db_view, ability);
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
	stats->town_center = stats->name == "TownCenter";
	// XR fork W4: values of the data (engine.xr, modpack tcfix4)
	if (genie.present) {
		stats->unit_class = genie.unit_class;
		stats->accuracy = std::clamp(genie.accuracy, 0.0, 100.0);
		stats->defense_terrain = genie.defense_terrain;
		if (genie.garrison_capacity > 0) {
			stats->garrison_capacity = genie.garrison_capacity;
			stats->garrison_mask = genie.garrison_mask;
			stats->garrison_heal = genie.garrison_heal;
		}
	}
	else {
		// older modpacks: classes from the types, town centers take 15 villagers/infantry/monks
		stats->unit_class = stats->villager ? 4 : stats->building ? 3 : -1;
		if (stats->town_center and stats->garrison_capacity == 0) {
			stats->garrison_capacity = 15;
		}
		if (stats->garrison_capacity > 0) {
			stats->garrison_mask = 11;
			stats->garrison_heal = 0.1;
		}
	}
	if (not stats->building) {
		// only buildings take a garrison here (rams and transport ships: W6)
		stats->garrison_capacity = 0;
		stats->garrison_mask = 0;
	}
	if (shoot.present) {
		// arrows per volley: the larger base of the API and of the data field (the converter
		// writes 0 for the town center, the data has 1 base arrow, 05 §5.4)
		stats->min_projectiles = std::max(shoot.min_projectiles, genie.min_projectiles);
		stats->max_projectiles = std::max({shoot.max_projectiles, genie.max_projectiles, stats->min_projectiles});
		if (stats->building and stats->max_projectiles <= 0) {
			stats->can_attack = false;
		}
		if (stats->building and stats->min_projectiles <= 0 and stats->garrison_capacity <= 0) {
			// no base arrow and no garrison: never shoots
			stats->can_attack = false;
		}
	}
	return stats;
}

} // namespace openage::gamestate::combat
