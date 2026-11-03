// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>


namespace openage::gamestate::special {

/**
 * Rules of the special abilities (XR fork W6, docs/aoe2-spec 05 §5.5, §5.7, §5.8):
 * monk conversions with faith, healing and area (splash) damage. Written from
 * the specification; every number that comes from the nyan data or from a
 * player resource is a parameter, the constants below are the ones the
 * specification names (docs/aoe2-spec/05-kampf.md). The decisions L45/L46 of
 * docs/PLAN-W5-W6.md close the open gaps (starting a conversion needs full
 * faith, healable targets).
 *
 * This header has no engine dependencies (host test gamestate/special_check.cpp).
 */

// ---------------------------------------------------------------- conversion

/// inputs of one conversion attempt (spec 05 §5.7 step 2); the resource values
/// are the player resources of the target owner and of the monk owner at that
/// moment, the times are game seconds
struct ConvertParams {
	/// random number of the attempt, z ∈ [0, 100]
	double z;
	/// target is a ship (classes 2, 20, 21, 22, 53)
	bool target_ship;
	/// target is on the building level (unit level 2/10: economic/military
	/// building stage, observed over the load order)
	bool target_building_level;
	/// target is of the scout/eagle warrior line
	bool target_scout;
	/// target owner's player resource 77 ("conversion resistance", e.g. Faith +3)
	double resist_77;
	/// target is a building (class 3) or a tower (52): the building window applies
	bool target_building_tower;
	/// monk owner's player resource 182 (building conversion chance, base 25)
	double res_182;
	/// monk owner's player resources 180/181 (min/max conversion time of
	/// buildings, base 15 s / 25 s)
	double res_180;
	double res_181;
	/// "accuracy" of the monk (unit conversion chance, base 25)
	double accuracy;
	/// work values 1/2 of the conversion task (min/max conversion time of
	/// units, base 4 s / 10 s)
	double work_min;
	double work_max;
	/// target owner's player resources 178/179 (time resistance min/max)
	double res_178;
	double res_179;
	/// monk owner's player resources 176/177 (time adjustment min/max)
	double res_176;
	double res_177;
	/// faith time t of this monk on this target (game seconds since the prayer began)
	double t;
};

/// resistance factor w (spec 05 §5.7 step 2): 0 by default; ships and units on
/// the building level set it to 3, the scout line adds 8, the target owner's
/// player resource 77 is added on top
inline double resist_factor(bool target_ship, bool target_building_level, bool target_scout, double resist_77) {
	double w = 0.0;
	if (target_ship or target_building_level) {
		w = 3.0;
	}
	if (target_scout) {
		w += 8.0;
	}
	return w + resist_77;
}

/// chance and time window of a conversion (spec 05 §5.7 step 2)
struct ConvertWindow {
	/// success threshold q: an attempt in the open window succeeds when z·w ≤ q
	double q;
	/// earliest time an attempt can succeed (T_min)
	double t_min;
	/// time from which an attempt surely succeeds (T_max)
	double t_max;
};

/// chance q and time window (spec 05 §5.7 step 2): buildings and towers use
/// the monk owner's resources 182 (chance) and 180/181 (times), all other
/// targets the accuracy of the monk and the work values of the task; the time
/// resistances of the target owner (178/179) and the adjustments of the monk
/// owner (176/177) add to the times
inline ConvertWindow window(const ConvertParams &p) {
	ConvertWindow win{};
	if (p.target_building_tower) {
		win.q = p.res_182;
		win.t_min = p.res_180;
		win.t_max = p.res_181;
	}
	else {
		win.q = p.accuracy;
		win.t_min = p.work_min;
		win.t_max = p.work_max;
	}
	win.t_min += p.res_178 + p.res_176;
	win.t_max += p.res_179 + p.res_177;
	return win;
}

/// outcome of a conversion attempt
enum class convert_t : uint8_t {
	/// before T_min: no success possible
	TOO_EARLY,
	/// in the open window with z·w > q
	FAIL,
	/// in the open window with z·w ≤ q, or from T_max on
	SUCCESS,
};

/// one conversion attempt (spec 05 §5.7 step 2, in that order): a positive
/// resistance factor multiplies z, before T_min nothing succeeds, from T_max on
/// everything succeeds, in between the attempt succeeds when z ≤ q
inline convert_t attempt(const ConvertParams &p) {
	double z = p.z;
	const double w = resist_factor(p.target_ship, p.target_building_level, p.target_scout, p.resist_77);
	if (w > 0.0) {
		z *= w;
	}
	const ConvertWindow win = window(p);
	if (p.t < win.t_min) {
		return convert_t::TOO_EARLY;
	}
	if (p.t >= win.t_max) {
		return convert_t::SUCCESS;
	}
	return z <= win.q ? convert_t::SUCCESS : convert_t::FAIL;
}

/// result of a successful conversion (spec 05 §5.7 step 3): true when the
/// target changes its owner; with heresy (target owner's player resource 192
/// > 0) it dies instead
inline bool converts_or_dies(bool heresy) {
	return not heresy;
}

/// whether a monk may start a conversion (decision L45, docs/PLAN-W5-W6.md):
/// only with full faith; healing is always allowed
inline bool may_start_conversion(double faith) {
	return faith >= 100.0;
}

/// whether all monks working on a converted target lose their faith (spec 05
/// §5.7 step 3): everyone without theocracy (monk owner's player resource 193
/// > 0), with it only the successful monk
inline bool faith_reset_all(bool theocracy) {
	return not theocracy;
}

// ---------------------------------------------------------------- faith

/// faith of a monk (resource type 34, range 0–100) after dt game seconds: it
/// rises by the owner's player resource 35 per second (base 1.6, full faith
/// after 62.5 s) and stays inside the range
inline double faith_after(double faith, double rate_per_s, double dt) {
	return std::clamp(faith + rate_per_s * dt, 0.0, 100.0);
}

// ---------------------------------------------------------------- healing

/// share of the heal rate when n monks heal the same target (spec 05 §5.8):
/// (1 + 0.5·(n-1)) / n; n >= 1
inline double heal_share(int n) {
	return (1.0 + 0.5 * (n - 1)) / n;
}

/// heal rate of one monk (spec 05 §5.8): work rate (base 1.25) × heal task
/// (base 2) × dt × heal share × heal factor; the heal factor is the owner's
/// player resource 89, a value ≤ 0 counts as 1
inline double heal_hp(double work_rate, double heal_task, double dt, int healers, double heal_factor) {
	if (heal_factor <= 0.0) {
		heal_factor = 1.0;
	}
	return work_rate * heal_task * dt * heal_share(healers) * heal_factor;
}

/// whether a target can be healed (decision L46, docs/PLAN-W5-W6.md): living
/// units except siege and ships; buildings never (spec 05 §5.8)
inline bool healable(bool alive, bool is_building, bool is_ship, bool is_siege) {
	return alive and not is_building and not is_ship and not is_siege;
}

// ---------------------------------------------------------------- area damage

/// whether an object stands in the area of an impact (spec 05 §5.5): the
/// bounding box of the object (centre x, y, half extents rx, ry) is nearer than
/// rho to the impact point x0, y0 (strictly, on the diagonal)
inline bool in_area(double x, double y, double rx, double ry, double x0, double y0, double rho) {
	const double dx = std::max(0.0, std::abs(x - x0) - rx);
	const double dy = std::max(0.0, std::abs(y - y0) - ry);
	return dx * dx + dy * dy < rho * rho;
}

/// area level filter (spec 05 §5.5): an object is hit when its defense level
/// is above 1 or when the attack level of the attacker is not above it
inline bool area_level_hit(int defense_level, int attack_level) {
	return defense_level > 1 or attack_level <= defense_level;
}

/// kind of an area attack (spec 05 §5.5)
enum class splash_t : uint8_t {
	/// projectile area damage (mangonel, onager): linear falloff
	PROJECTILE,
	/// war elephants (normal/elite): side targets take half the damage
	ELEPHANT,
	/// cataphracts and infantry with an area attack: side targets lose a flat 5 HP
	FLAT5,
	/// rams: buildings only besides the main target
	RAM,
	/// demolition ships/rafts: linear falloff like projectiles
	DEMOLITION,
};

/// side targets of a flat area attack lose this many HP (spec 05 §5.5)
constexpr double FLAT5_HP = 5.0;

/// share of the damage that side targets of war elephants take (spec 05 §5.5)
constexpr double ELEPHANT_SHARE = 0.5;

/// damage an object takes from an area attack (spec 05 §5.5): the main target
/// always takes the full damage; side targets per kind — projectiles and
/// demolition ships attenuated linearly towards the edge of the area
/// ((rho - distance) / rho, never negative), elephants half, flat area attacks
/// a flat 5 HP, rams buildings only. distance is the box distance of in_area()
/// (side targets are nearer than rho), rho > 0.
inline double splash_damage(splash_t kind, double main_damage, double distance, double rho,
                            bool is_main_target, bool target_is_building) {
	if (is_main_target) {
		return main_damage;
	}
	switch (kind) {
	case splash_t::RAM:
		return target_is_building ? main_damage : 0.0;
	case splash_t::FLAT5:
		return FLAT5_HP;
	case splash_t::ELEPHANT:
		return main_damage * ELEPHANT_SHARE;
	case splash_t::PROJECTILE:
	case splash_t::DEMOLITION:
		if (rho <= 0.0) {
			return 0.0;
		}
		return main_damage * std::max(0.0, (rho - distance) / rho);
	}
	return 0.0;
}

/// whether an area attack spares own and allied objects (spec 05 §5.5): melee
/// area attacks (range < 1, e.g. war elephants) do, ranged ones (mangonel) do not
inline bool spares_friendly(bool melee_area) {
	return melee_area;
}

} // namespace openage::gamestate::special
