// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the special rules (XR fork W6, gamestate/special_math.h):
 *
 *   openage-special-check
 *
 * Parameter sweeps:
 * - conversion: z × faith time × target kind (too early / sure success /
 *   threshold), the resistance factor multiplies z (ships/building level 3,
 *   scout line +8, resource 77), buildings and towers use their own chance and
 *   times, the time resistances (178/179) and adjustments (176/177) shift the
 *   window, heresy kills instead of converting, theocracy spares the other
 *   monks, start only with full faith (L45)
 * - faith: rises with the rate, stays inside 0–100 whatever the step size
 * - healing: share for 1–20 healers, heal rate with the base values, heal
 *   factor ≤ 0 counts as 1, healable targets (L46)
 * - area damage: point inside/outside/on the border of the extended rectangle,
 *   linear falloff for projectiles and demolition ships, main target always
 *   full, rams hit no unit as a side target, elephants half, flat 5 HP, level
 *   filter, melee area attacks spare own units
 * Exit code 0 if all checks pass. No dependencies besides the standard library.
 */

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "gamestate/special_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::special;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string &what) {
	checks += 1;
	if (not ok) {
		failures += 1;
		if (failures <= 40) {
			std::cerr << "FAIL: " << what << std::endl;
		}
	}
}

bool near(double a, double b, double eps = 1e-9) {
	return std::abs(a - b) <= eps;
}

/// base monk against a plain unit: chance 25, window 4–10 s (spec 05 §5.7)
ConvertParams unit_attempt(double z, double t) {
	ConvertParams p{};
	p.z = z;
	p.target_ship = false;
	p.target_building_level = false;
	p.target_scout = false;
	p.resist_77 = 0.0;
	p.target_building_tower = false;
	p.res_182 = 25.0;
	p.res_180 = 15.0;
	p.res_181 = 25.0;
	p.accuracy = 25.0;
	p.work_min = 4.0;
	p.work_max = 10.0;
	p.res_178 = 0.0;
	p.res_179 = 0.0;
	p.res_176 = 0.0;
	p.res_177 = 0.0;
	p.t = t;
	return p;
}

void check_conversion() {
	// time × chance: before T_min nothing succeeds, from T_max on everything,
	// in between the attempt succeeds when z ≤ q
	for (double t : {0.0, 2.0, 3.999, 4.0, 5.0, 7.0, 9.999, 10.0, 12.0}) {
		for (double z = 0.0; z <= 100.0; z += 0.5) {
			const convert_t got = attempt(unit_attempt(z, t));
			convert_t want = convert_t::FAIL;
			if (t < 4.0) {
				want = convert_t::TOO_EARLY;
			}
			else if (t >= 10.0) {
				want = convert_t::SUCCESS;
			}
			else if (z <= 25.0) {
				want = convert_t::SUCCESS;
			}
			check(got == want, "unit conversion z=" + std::to_string(z) + " t=" + std::to_string(t));
		}
	}
	const ConvertWindow win = window(unit_attempt(50.0, 0.0));
	check(near(win.q, 25.0) and near(win.t_min, 4.0) and near(win.t_max, 10.0), "unit window");

	// resistance factor: ships and the building level set 3 (also together),
	// the scout line adds 8, resource 77 adds on top
	check(near(resist_factor(false, false, false, 0.0), 0.0), "resistance base 0");
	check(near(resist_factor(true, false, false, 0.0), 3.0), "resistance ship 3");
	check(near(resist_factor(false, true, false, 0.0), 3.0), "resistance building level 3");
	check(near(resist_factor(true, true, false, 0.0), 3.0), "ship and building level still 3");
	check(near(resist_factor(false, false, true, 0.0), 8.0), "resistance scout 8");
	check(near(resist_factor(true, false, true, 0.0), 11.0), "resistance ship+scout 11");
	check(near(resist_factor(false, false, false, 3.0), 3.0), "resource 77 adds");
	check(near(resist_factor(true, true, true, 3.0), 14.0), "everything: 3+8+3");
	// a positive resistance multiplies z: 9 stays under the chance 25, ships
	// push it over; w = 0 leaves z untouched
	ConvertParams p = unit_attempt(9.0, 7.0);
	check(attempt(p) == convert_t::SUCCESS, "no resistance: 9 <= 25");
	p.target_ship = true;
	check(attempt(p) == convert_t::FAIL, "ship resistance: 27 > 25");
	p.target_ship = false;
	p.target_scout = true;
	check(attempt(p) == convert_t::FAIL, "scout resistance: 72 > 25");
	p.target_scout = false;
	p.resist_77 = 2.0;
	check(attempt(p) == convert_t::SUCCESS, "resource 77: 18 <= 25");
	p.resist_77 = 3.0;
	check(attempt(p) == convert_t::FAIL, "resource 77: 27 > 25");
	check(attempt(unit_attempt(100.0, 7.0)) == convert_t::FAIL, "w = 0 keeps z");

	// buildings and towers: chance 182 (here 30), times 180/181 (15 s/25 s),
	// the unit values stay unused
	p = unit_attempt(0.0, 0.0);
	p.target_building_tower = true;
	p.res_182 = 30.0;
	p.res_180 = 15.0;
	p.res_181 = 25.0;
	p.accuracy = 99.0;
	const ConvertWindow bwin = window(p);
	check(near(bwin.q, 30.0) and near(bwin.t_min, 15.0) and near(bwin.t_max, 25.0), "building window");
	check(attempt(p) == convert_t::TOO_EARLY, "building too early (unit window would allow)");
	p.t = 15.0;
	p.z = 30.0;
	check(attempt(p) == convert_t::SUCCESS, "building chance at the threshold");
	p.z = 30.5;
	check(attempt(p) == convert_t::FAIL, "building over the chance (unit accuracy unused)");
	p.t = 25.0;
	p.z = 100.0;
	check(attempt(p) == convert_t::SUCCESS, "building sure from T_max");

	// times: the target owner's resistances (178/179) and the monk owner's
	// adjustments (176/177) add, min to T_min and max to T_max
	p = unit_attempt(0.0, 0.0);
	p.res_178 = 2.0;
	p.res_176 = 1.0;
	p.t = 6.999;
	check(attempt(p) == convert_t::TOO_EARLY, "T_min 4+2+1");
	p.t = 7.0;
	check(attempt(p) == convert_t::SUCCESS, "at T_min with z = 0");
	p = unit_attempt(100.0, 0.0);
	p.res_179 = 3.0;
	p.res_177 = 2.0;
	p.t = 14.999;
	check(attempt(p) == convert_t::FAIL, "T_max 10+3+2 not reached");
	p.t = 15.0;
	check(attempt(p) == convert_t::SUCCESS, "sure at T_max");
	p.target_building_tower = true;
	p.res_180 = 15.0;
	p.res_181 = 25.0;
	p.res_178 = 2.0;
	p.res_176 = 1.0;
	p.t = 17.999;
	p.z = 0.0;
	check(attempt(p) == convert_t::TOO_EARLY, "building T_min 15+2+1");
	p.t = 18.0;
	check(attempt(p) == convert_t::SUCCESS, "building at the shifted T_min");
	p.t = 29.999;
	p.z = 100.0;
	check(attempt(p) == convert_t::FAIL, "building T_max 25+3+2 not reached");
	p.t = 30.0;
	check(attempt(p) == convert_t::SUCCESS, "building sure at the shifted T_max");

	// success, heresy, theocracy, start of a conversion (L45)
	check(converts_or_dies(false), "success converts the target");
	check(not converts_or_dies(true), "heresy kills the target");
	check(may_start_conversion(100.0), "full faith starts a conversion");
	check(not may_start_conversion(99.999), "not quite full faith starts none");
	check(not may_start_conversion(0.0), "empty faith starts none");
	check(faith_reset_all(false), "all monks on the target lose their faith");
	check(not faith_reset_all(true), "theocracy: only the successful monk");
}

void check_faith() {
	check(near(faith_after(0.0, 1.6, 62.5), 100.0), "base rate 1.6/s fills in 62.5 s");
	check(near(faith_after(99.0, 10.0, 1.0), 100.0), "clamped at 100");
	check(near(faith_after(100.0, 1.6, 5.0), 100.0), "full faith stays 100");
	check(near(faith_after(1.0, 5.0, 0.1), 1.5), "linear rise");
	check(near(faith_after(0.5, -10.0, 1.0), 0.0), "clamped at 0");
	// the same total time reaches the same value, whatever the step size
	for (double dt : {62.5, 12.5, 0.5, 0.03125}) {
		double faith = 0.0;
		for (double t = 0.0; t < 62.5; t += dt) {
			faith = faith_after(faith, 1.6, dt);
		}
		check(near(faith, 100.0, 1e-6), "step size does not matter (filled)");
	}
	for (double dt : {40.0, 10.0, 2.5, 0.625}) {
		double faith = 0.0;
		for (double t = 0.0; t < 40.0; t += dt) {
			faith = faith_after(faith, 1.0, dt);
		}
		check(near(faith, 40.0, 1e-6), "step size does not matter (below 100)");
	}
}

void check_healing() {
	// share: (1 + 0.5·(n-1))/n = 0.5 + 1/(2n), decreasing, above one half
	check(near(heal_share(1), 1.0), "one healer heals the full share");
	check(near(heal_share(2), 0.75), "two healers: 1.5/2");
	check(near(heal_share(3), 2.0 / 3.0), "three healers: 2/3");
	check(near(heal_share(20), 0.525), "twenty healers: 10.5/20");
	double last = heal_share(1);
	for (int n = 2; n <= 20; n++) {
		const double share = heal_share(n);
		check(share < last, "share decreases with more healers");
		check(share > 0.5, "share stays above half");
		check(near(share, 0.5 + 0.5 / n), "share closed form");
		last = share;
	}
	// rate: W·a1·dt·share·H with the base values W = 1.25, a1 = 2, H = 1
	check(near(heal_hp(1.25, 2.0, 1.0, 1, 0.0), 2.5), "heal factor <= 0 counts as 1");
	check(near(heal_hp(1.25, 2.0, 1.0, 1, 1.0), 2.5), "base heal rate 2.5 HP/s");
	check(near(heal_hp(1.25, 2.0, 1.0, 1, 2.0), 5.0), "heal factor doubles the rate");
	check(near(heal_hp(1.25, 2.0, 1.0, 2, 1.0), 1.875), "two healers share the rate");
	check(near(heal_hp(1.25, 2.0, 0.5, 1, 1.0), 1.25), "half a second heals half");
	for (int n = 1; n <= 20; n++) {
		for (double dt : {0.25, 0.5, 1.0}) {
			for (double h : {0.0, 1.0, 2.5}) {
				const double want = 2.5 * dt * (0.5 + 0.5 / n) * (h <= 0.0 ? 1.0 : h);
				check(near(heal_hp(1.25, 2.0, dt, n, h), want), "heal rate sweep");
			}
		}
	}
	// healable targets (L46): living units except siege and ships, buildings never
	check(healable(true, false, false, false), "living unit is healable");
	check(not healable(false, false, false, false), "dead is not");
	check(not healable(true, true, false, false), "building is not");
	check(not healable(true, false, true, false), "ship is not");
	check(not healable(true, false, false, true), "siege is not");
	for (int bits = 0; bits < 8; bits++) {
		const bool building = bits & 1;
		const bool ship = bits & 2;
		const bool siege = bits & 4;
		for (bool alive : {false, true}) {
			const bool want = alive and not building and not ship and not siege;
			check(healable(alive, building, ship, siege) == want, "healable sweep");
		}
	}
}

void check_area_damage() {
	// in_area: box distance strictly nearer than rho, also at the border
	check(in_area(5.0, 5.0, 0.5, 0.5, 5.0, 5.0, 0.1), "impact on the centre");
	check(in_area(5.0, 5.0, 0.5, 0.5, 5.2, 5.3, 0.1), "impact inside the box");
	check(in_area(5.0, 5.0, 0.5, 0.5, 6.0, 5.0, 1.0), "box distance 0.5 < rho 1");
	check(not in_area(5.0, 5.0, 0.5, 0.5, 6.0, 5.0, 0.5), "border: box distance = rho");
	check(not in_area(5.0, 5.0, 0.5, 0.5, 6.25, 6.5, 1.25), "corner border: 0.75²+1² = 1.25²");
	check(in_area(5.0, 5.0, 0.5, 0.5, 6.25, 6.49, 1.25), "just inside the corner");
	check(not in_area(5.0, 5.0, 0.5, 0.5, 10.0, 10.0, 3.0), "far outside");
	check(not in_area(5.0, 5.0, 0.5, 0.5, 5.0, 5.0, 0.0), "rho 0 hits nothing (strict)");
	for (double x0 = 3.0; x0 <= 7.0; x0 += 0.25) {
		for (double y0 = 3.0; y0 <= 7.0; y0 += 0.25) {
			const double dx = std::max(0.0, std::abs(5.0 - x0) - 0.5);
			const double dy = std::max(0.0, std::abs(5.0 - y0) - 0.5);
			const bool want = std::sqrt(dx * dx + dy * dy) < 1.25;
			check(in_area(5.0, 5.0, 0.5, 0.5, x0, y0, 1.25) == want, "grid sweep against the distance form");
		}
	}

	// level filter: defense above 1, or attack not above defense
	for (int defense = 0; defense <= 3; defense++) {
		for (int attack = 0; attack <= 3; attack++) {
			const bool want = defense > 1 or attack <= defense;
			check(area_level_hit(defense, attack) == want, "level filter sweep");
		}
	}
	check(area_level_hit(0, 0), "equal levels hit");
	check(not area_level_hit(0, 1), "attack above defense 0");
	check(not area_level_hit(1, 2), "attack above defense 1");
	check(area_level_hit(2, 3), "defense above 1 always hit");

	// the main target always takes the full damage, whatever the kind
	for (auto kind : {splash_t::PROJECTILE, splash_t::ELEPHANT, splash_t::FLAT5, splash_t::RAM, splash_t::DEMOLITION}) {
		check(near(splash_damage(kind, 10.0, 0.0, 1.5, true, false), 10.0), "main target full");
		check(near(splash_damage(kind, 10.0, 5.0, 1.5, true, false), 10.0), "main target ignores the distance");
	}
	// rams: buildings only besides the main target
	check(near(splash_damage(splash_t::RAM, 8.0, 0.5, 1.0, true, false), 8.0), "ram main target unit");
	check(near(splash_damage(splash_t::RAM, 8.0, 0.5, 1.0, false, false), 0.0), "ram spares units");
	check(near(splash_damage(splash_t::RAM, 8.0, 0.5, 1.0, false, true), 8.0), "ram hits buildings");
	// projectiles and demolition ships: linear falloff towards the edge
	for (auto kind : {splash_t::PROJECTILE, splash_t::DEMOLITION}) {
		for (double d = 0.0; d <= 2.0; d += 0.25) {
			check(near(splash_damage(kind, 10.0, d, 2.0, false, false), 10.0 * (2.0 - d) / 2.0),
			      "linear falloff");
		}
		check(near(splash_damage(kind, 10.0, 2.0, 2.0, false, false), 0.0), "edge of the area");
		check(near(splash_damage(kind, 10.0, 2.5, 2.0, false, false), 0.0), "never negative");
		check(near(splash_damage(kind, 10.0, 1.0, 0.0, false, false), 0.0), "rho 0: no side damage");
	}
	// elephants half, flat attacks a flat 5 HP: independent of damage, distance, rho
	check(near(splash_damage(splash_t::ELEPHANT, 13.0, 0.3, 1.0, false, false), 6.5), "elephant half");
	check(near(splash_damage(splash_t::FLAT5, 13.0, 0.3, 1.0, false, false), 5.0), "flat 5 HP");
	check(near(splash_damage(splash_t::FLAT5, 100.0, 1.9, 2.0, false, false), 5.0), "flat ignores the damage");
	check(near(splash_damage(splash_t::ELEPHANT, 13.0, 1.9, 2.0, false, false), 6.5), "elephant ignores the distance");

	// friendly fire: melee area attacks spare own units, ranged ones do not
	check(spares_friendly(true), "melee area spares own units");
	check(not spares_friendly(false), "ranged area hits own units");
}

} // namespace

int main() {
	check_conversion();
	check_faith();
	check_healing();
	check_area_damage();
	if (failures > 0) {
		std::cout << "special check: " << checks << " checks, " << failures << " failures" << std::endl;
		return EXIT_FAILURE;
	}
	std::cout << "special check: " << checks << " checks, 0 failures" << std::endl;
	return EXIT_SUCCESS;
}
