// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nyan/nyan.h>

#include "gamestate/combat/rules.h"


namespace openage::gamestate::combat {

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
