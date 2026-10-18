// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include <nyan/nyan.h>

#include "gamestate/combat/rules.h"


namespace openage::gamestate::combat {

/**
 * Flight values of a projectile (XR fork W4, docs/aoe2-spec 05 §5.3), read from
 * the projectile game entity: Move.speed, Projectile.arc (launch angle in
 * degrees, converted from the data field "projectile arc"), the first
 * Projectile.accuracy (accuracy %, dispersion), Projectile.target_mode
 * (ExpectedPosition = lead, patched by ballistics) and the raw penetration
 * mode of engine.xr GenieUnitData.
 */
struct ProjectileStats {
	double speed = 7.0;
	double arc = 0.0;
	double accuracy = 100.0;
	double dispersion = 0.0;
	bool lead = false;
	bool pierce = false;
	/// damage of this projectile (ApplyDiscreteEffect of the projectile)
	AttackProfile attack{};
};

/**
 * Combat values of a game entity type, read once from nyan (XR fork).
 */
struct CombatStats {
	/// short name for logs (last part of the fqon)
	std::string name;

	// GameEntity.types
	bool unit = false;
	bool building = false;
	bool villager = false;
	/// Ambient (trees, mines, bushes) and herdables/prey are never attacked
	/// automatically and do not count for the victory condition
	bool ambient = false;
	bool herdable = false;

	/// has Live with a health attribute
	bool alive = false;
	/// attribute fqon of the health (Live.attributes)
	nyan::fqon_t health_attribute;
	int64_t max_health = 0;
	int64_t start_health = 0;

	/// ApplyDiscreteEffect or ShootProjectile with at least one projectile
	bool can_attack = false;
	/// ShootProjectile (instant hit at range, no projectile flight yet)
	bool ranged = false;
	AttackProfile attack{};
	double reload_time = 2.0;
	double min_range = 0.0;
	/// 0 = melee
	double max_range = 0.0;

	ArmorProfile armor{};

	double line_of_sight = DEFAULT_LINE_OF_SIGHT;
	/// Collision hitbox (tiles)
	double radius = 0.25;
	double height = 1.0;

	/// animation paths (empty if none)
	std::string attack_animation;
	std::string death_animation;
	/// duration of the death animation before the entity is removed (s)
	double death_time = 0.0;

	/// the entity can move (Move ability)
	bool movable = false;
	/// Move.speed (tiles/s), 0 if it cannot move
	double speed = 0.0;

	// ---- XR fork W4 (docs/aoe2-spec 05, 06, 07)

	/// first projectile of ShootProjectile and the second one (extra garrison arrows), if any
	ProjectileStats projectile{};
	bool has_secondary = false;
	ProjectileStats secondary{};
	/// arrows per volley: base (largest of ShootProjectile.min_projectiles and the data field
	/// "projectile min count") and maximum
	int min_projectiles = 0;
	int max_projectiles = 0;
	/// spawning area of the extra arrows (width across the shot, length along it) and the
	/// launch height (ShootProjectile.spawning_area_offset_z)
	double spawn_width = 0.0;
	double spawn_length = 0.0;
	double spawn_height = 1.0;
	/// hit roll of melee attacks (data field "accuracy", 100 if missing)
	double accuracy = 100.0;
	/// unit class of the data (GenieUnitData.unit_class, -1 = unknown)
	int unit_class = -1;
	/// garrison: places, type mask, healing (HP/s) – largest over the data variants
	int garrison_capacity = 0;
	int garrison_mask = 0;
	double garrison_heal = 0.0;
	/// GameEntityStance ability (military units choose a stance)
	bool stances = false;
	/// Formation ability and the ordering priority of its subformation (front rows high)
	bool formations = false;
	int formation_priority = 0;
	/// elevation modifiers of the entity or its projectile (05 §5.2): multiplier from above
	double elevation_high = 1.25;
	/// damage terrain restriction (data field "defense terrain bonus", -1 = none)
	int defense_terrain = -1;
	/// the town center (garrison arrows, town bell)
	bool town_center = false;

	/// attack of one hit divided by the reload time (garrison arrows, 05 §5.4); ranged
	/// units: the pierce attack of the projectile
	double dps() const {
		int64_t sum = 0;
		for (const auto &e : this->attack.effects) {
			if (not ends_with(e.type, FALLBACK_SUFFIX)) {
				sum = std::max(sum, e.amount);
			}
		}
		return this->reload_time > 0.0 ? static_cast<double>(sum) / this->reload_time : 0.0;
	}

	/// the building shoots arrows (with or without garrison)
	bool shoots_arrows() const {
		return this->building and this->ranged and this->max_projectiles > 0;
	}

	/// military unit (stances, formations, patrol …)
	bool military() const {
		return this->unit and not this->building and this->can_attack and this->movable and not this->villager;
	}

	/// counts for the victory condition
	bool counts_for_victory() const {
		return this->alive and (this->unit or this->building) and not this->ambient and not this->herdable;
	}

	/// can be a target of attacks
	bool attackable() const {
		return this->alive and (this->unit or this->building) and not this->ambient;
	}
};

/**
 * Read the combat values of a game entity type.
 *
 * Damage: effects of ApplyDiscreteEffect.batches (melee) or of the first
 * projectile of ShootProjectile.projectiles (ranged), each
 * FlatAttributeChangeDecrease with its change type and amount. Armor:
 * Resistance.resistances (FlatAttributeChangeDecrease block values).
 *
 * @param db_view nyan view of the owner.
 * @param entity_fqon GameEntity object.
 *
 * @return Combat values (defaults for missing abilities, never throws for
 *         missing members).
 */
std::shared_ptr<const CombatStats> read_combat_stats(const std::shared_ptr<nyan::View> &db_view,
                                                     const nyan::fqon_t &entity_fqon);

} // namespace openage::gamestate::combat
