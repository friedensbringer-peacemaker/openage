// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the diplomacy rules (XR fork W7, gamestate/diplomacy_math.h):
 *
 *   openage-diplomacy-check
 *
 * Truth tables over every combination of the bool parameters and the
 * stances: auto target choice (16 target/attacker rows per stance),
 * ordered attacks, stray projectiles (owner, stance, gaia and three
 * defense/attack level pairs), shared vision of the resources (0, 0.5,
 * 50) and the tribute button (stock 0/50/1000, shift, ctrl, never
 * negative).
 * Exit code 0 if all checks pass. No dependencies besides the standard library.
 */

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "gamestate/diplomacy_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::diplomacy;

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

bool near(double a, double b) {
	return std::abs(a - b) <= 1e-9;
}

/// row index of one combination: bit 3 building, 2 military, 1 villager, 0 scout
int row(bool building, bool military, bool villager, bool scout) {
	return (building ? 8 : 0) + (military ? 4 : 0) + (villager ? 2 : 0) + (scout ? 1 : 0);
}

void check_auto_target() {
	// truth tables of auto_target_allowed, one char per row 0..15
	const std::array<const char *, 3> names = {"ALLY", "NEUTRAL", "ENEMY"};
	const std::array<const char *, 3> table = {
		"0000000000000000", // ALLY: allies are never attacked
		"0000110011001100", // NEUTRAL: buildings and military, never villagers
		"1110111011101110", // ENEMY: all, but no scout picks villagers
	};
	const std::array<stance_t, 3> stances = {stance_t::ALLY, stance_t::NEUTRAL, stance_t::ENEMY};
	for (int s = 0; s < 3; ++s) {
		for (int r = 0; r < 16; ++r) {
			const bool got = auto_target_allowed(
				stances[s], (r & 8) != 0, (r & 4) != 0, (r & 2) != 0, (r & 1) != 0);
			check(got == (table[s][r] == '1'),
			      std::string("auto target ") + names[s] + " row " + std::to_string(r));
		}
	}
	check(row(true, false, false, false) == 8, "row bits are building, military, villager, scout");

	// ordered attacks: only allies are forbidden
	check(not ordered_attack_allowed(stance_t::ALLY), "no ordered attack on an ally");
	check(ordered_attack_allowed(stance_t::NEUTRAL), "ordered attack on neutral is allowed");
	check(ordered_attack_allowed(stance_t::ENEMY), "ordered attack on enemy is allowed");
}

void check_stray_projectile() {
	// every combination of owner, stance and gaia with (defense, attack) pairs
	const std::array<stance_t, 3> stances = {stance_t::ALLY, stance_t::NEUTRAL, stance_t::ENEMY};
	const int levels[3][2] = {{3, 3}, {2, 5}, {5, 2}};
	for (int same = 0; same < 2; ++same) {
		for (int s = 0; s < 3; ++s) {
			for (int gaia = 0; gaia < 2; ++gaia) {
				for (int l = 0; l < 3; ++l) {
					const bool got = stray_projectile_may_hit(
						same == 1, stances[s], gaia == 1, levels[l][0], levels[l][1]);
					const bool expect = same == 0 and s != 0 and gaia == 0
					                    and levels[l][0] >= levels[l][1];
					check(got == expect, "stray projectile combination");
				}
			}
		}
	}
	// hand computed examples (spec 05)
	check(not stray_projectile_may_hit(true, stance_t::ENEMY, false, 3, 3), "own objects are safe");
	check(not stray_projectile_may_hit(false, stance_t::ALLY, false, 3, 3), "allies are safe");
	check(not stray_projectile_may_hit(false, stance_t::NEUTRAL, true, 5, 2), "gaia trees are ignored");
	check(stray_projectile_may_hit(false, stance_t::ENEMY, false, 3, 3), "equal level is hit");
	check(not stray_projectile_may_hit(false, stance_t::ENEMY, false, 2, 5), "weak defense stops");
	check(stray_projectile_may_hit(false, stance_t::ENEMY, false, 5, 2), "strong defense is hit");
}

void check_vision() {
	const std::array<stance_t, 3> stances = {stance_t::ALLY, stance_t::NEUTRAL, stance_t::ENEMY};
	const double reveal[3] = {0.0, 0.5, 50.0};
	for (int s = 0; s < 3; ++s) {
		for (int r = 0; r < 3; ++r) {
			check(shares_vision(stances[s], reveal[r]) == (s == 0 and reveal[r] > 0.0),
			      "shared vision combination");
		}
	}
	check(shares_vision(stance_t::ALLY, 50.0), "ally with reveal 50 shares the vision");
	check(not shares_vision(stance_t::NEUTRAL, 50.0), "neutral shares nothing");
	check(not shares_vision(stance_t::ALLY, 0.0), "reveal 0 shows nothing");
}

void check_tribute() {
	const double stocks[3] = {0.0, 50.0, 1000.0};
	for (int s = 0; s < 3; ++s) {
		check(near(tribute_click_amount(stocks[s], false, false), 100.0), "click sends 100");
		check(near(tribute_click_amount(stocks[s], true, false), 500.0), "shift sends 500");
		check(near(tribute_click_amount(stocks[s], false, true), stocks[s]), "ctrl sends the stock");
		check(near(tribute_click_amount(stocks[s], true, true), stocks[s]), "ctrl wins over shift");
	}
	check(near(tribute_click_amount(-25.0, false, true), 0.0), "never negative");
	check(near(tribute_click_amount(-25.0, true, false), 500.0), "fixed amounts stay positive");
}

} // namespace

int main() {
	check_auto_target();
	check_stray_projectile();
	check_vision();
	check_tribute();
	std::cout << "diplomacy check: " << checks << " checks, " << failures << " failures"
	          << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
