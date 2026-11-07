// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>


namespace openage::gamestate::score {

/**
 * Score of one player (XR fork W8, spec 13 L4): military, economy,
 * technology and society, each a percentage of what the player achieved.
 *
 * This header has no engine dependencies (host test gamestate/score_check.cpp).
 */

/// raw values of one player before the weights are applied (spec 13 L4)
struct ScoreInput {
	double destroyed_or_converted_value;
	double resources_owned;
	double tribute_paid;
	double remaining_value;
	double researched_tech_value;
	double explored_percent;
	double castle_wonder_cost;
};

/// rounded score of one player, total is the sum of the four parts
struct Score {
	int military;
	int economy;
	int technology;
	int society;
	int total;
};

/**
 * Weights of the four parts (spec 13 L4): military 20% of the destroyed
 * and converted value, economy 10% of resources plus tribute and 20% of
 * the remaining value, technology 20% of the researched techs plus 10
 * per percent explored, society 20% of the castle and wonder cost.
 * Each part is rounded down with std::floor (assumption, the spec does
 * not name the rounding); the total is the sum of the rounded parts.
 * The spec help line says remaining_value counts "with exceptions" (the
 * text is cut off): the caller decides what counts towards it.
 */
inline Score compute(const ScoreInput &in) {
	const int military = static_cast<int>(std::floor(0.20 * in.destroyed_or_converted_value));
	const int economy = static_cast<int>(
		std::floor(0.10 * (in.resources_owned + in.tribute_paid) + 0.20 * in.remaining_value));
	const int technology = static_cast<int>(
		std::floor(0.20 * in.researched_tech_value + 10.0 * in.explored_percent));
	const int society = static_cast<int>(std::floor(0.20 * in.castle_wonder_cost));
	return Score{military, economy, technology, society,
	             military + economy + technology + society};
}

/// index of the player with the star, that is the highest total: the
/// first one on a tie, -1 when the list is empty
inline int leader(const std::vector<Score> &scores) {
	int best = -1;
	for (size_t i = 0; i < scores.size(); ++i) {
		if (best < 0 or scores[i].total > scores[best].total) {
			best = static_cast<int>(i);
		}
	}
	return best;
}

} // namespace openage::gamestate::score
