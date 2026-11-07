// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <string>


namespace openage::gamestate::diplomacy {

/**
 * Rules of the diplomacy (XR fork W7, specs 05, 06 and 08): the stance
 * between two players, which targets a unit picks on its own, where a
 * stray projectile may land, that allied players share their vision of
 * the map (player resource 50) and how much one click on the tribute
 * button sends. Written from the specification; the fee of the tribute
 * itself is computed by trade::tribute, not here.
 *
 * This header has no engine dependencies (host test gamestate/diplomacy_check.cpp).
 */

/// stance of one player towards the owner of an object (spec 06)
enum class stance_t { ALLY, NEUTRAL, ENEMY };

/// word of a stance in save games
inline const char *stance_name(stance_t stance) {
	return stance == stance_t::ALLY ? "ally" : stance == stance_t::NEUTRAL ? "neutral" : "enemy";
}

/// stance of a word in a save game, false for an unknown word
inline bool parse_stance(const std::string &word, stance_t &stance) {
	if (word == "ally") {
		stance = stance_t::ALLY;
	}
	else if (word == "neutral") {
		stance = stance_t::NEUTRAL;
	}
	else if (word == "enemy") {
		stance = stance_t::ENEMY;
	}
	else {
		return false;
	}
	return true;
}

// ------------------------------------------------------------- target choice

/**
 * Auto target choice of a unit (spec 06): allies are never attacked. A
 * neutral player only loses buildings and military units, villagers stay
 * safe. Against an enemy everything is a target, only scouts do not pick
 * villagers on their own.
 */
inline bool auto_target_allowed(stance_t stance, bool target_is_building, bool target_is_military,
                                bool target_is_villager, bool attacker_is_scout) {
	if (stance == stance_t::ALLY) {
		return false;
	}
	if (stance == stance_t::NEUTRAL) {
		return (target_is_building or target_is_military) and not target_is_villager;
	}
	return not (attacker_is_scout and target_is_villager);
}

/// an ordered attack is allowed against everyone but allies (spec 06)
inline bool ordered_attack_allowed(stance_t stance) {
	return stance != stance_t::ALLY;
}

// --------------------------------------------------------- stray projectiles

/**
 * Missed shot of a projectile (spec 05): the projectile missed its
 * target, this is no area attack. It never hits the own objects and
 * never the objects of an ally; gaia objects that ignore stray shots
 * (trees, resources) are safe; an area is only hit while its defense
 * level is at least the attack level of the projectile.
 */
inline bool stray_projectile_may_hit(bool same_owner, stance_t stance_to_owner, bool gaia_ignored,
                                     int area_defense_level, int projectile_attack_level) {
	if (same_owner or stance_to_owner == stance_t::ALLY or gaia_ignored) {
		return false;
	}
	return area_defense_level >= projectile_attack_level;
}

// -------------------------------------------------------------------- vision

/// allied players share their vision of the map (spec 05, player resource 50 cartography)
inline bool shares_vision(stance_t stance_of_viewer_to_owner, double reveal_allies_resource_50) {
	return stance_of_viewer_to_owner == stance_t::ALLY and reveal_allies_resource_50 > 0.0;
}

// ------------------------------------------------------------ tribute button

/**
 * One click on the tribute button (spec 08): 100, with shift 500, with
 * ctrl the whole stock (ctrl wins over shift). Never negative.
 */
inline double tribute_click_amount(double stock, bool shift, bool ctrl) {
	if (ctrl) {
		return std::max(0.0, stock);
	}
	return shift ? 500.0 : 100.0;
}

} // namespace openage::gamestate::diplomacy
