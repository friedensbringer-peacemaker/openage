// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>


namespace openage::gamestate::stats {

/**
 * Columns of the end of game screen (XR fork W8, spec 13): the star for
 * the best player of a column and the population shares of the timeline.
 * This header has no engine dependencies (host test
 * gamestate/stats_check.cpp).
 */

/**
 * Star for the best player of a column. Every player with the highest
 * value gets the star; a highest value of 0 or below gives nobody the
 * star (assumption, the spec does not name this case).
 */
inline std::vector<bool> best_in_column(const std::vector<double> &values) {
	std::vector<bool> stars(values.size(), false);
	if (values.empty()) {
		return stars;
	}
	const double best = *std::max_element(values.begin(), values.end());
	if (best > 0.0) {
		for (size_t i = 0; i < values.size(); ++i) {
			stars[i] = values[i] == best;
		}
	}
	return stars;
}

/**
 * Share of every player in percent for the population timeline. The
 * shares add up to 100, negative populations count as 0 and all zero
 * gives only zeros.
 */
inline std::vector<double> population_shares(const std::vector<int> &population) {
	double total = 0.0;
	for (const int count : population) {
		if (count > 0) {
			total += count;
		}
	}
	std::vector<double> shares(population.size(), 0.0);
	if (total > 0.0) {
		for (size_t i = 0; i < population.size(); ++i) {
			if (population[i] > 0) {
				shares[i] = population[i] * 100.0 / total;
			}
		}
	}
	return shares;
}

/**
 * Game time as text: under one hour as M:SS, from one hour on as
 * H:MM:SS. Negative values count as 0, always rounded down to whole
 * seconds.
 */
inline std::string clock_text(double game_seconds) {
	if (game_seconds < 0.0) {
		game_seconds = 0.0;
	}
	const auto total = static_cast<unsigned long long>(std::floor(game_seconds));
	char text[32];
	if (total >= 3600) {
		std::snprintf(text, sizeof(text), "%llu:%02llu:%02llu",
		              total / 3600, (total / 60) % 60, total % 60);
	}
	else {
		std::snprintf(text, sizeof(text), "%llu:%02llu", total / 60, total % 60);
	}
	return text;
}

} // namespace openage::gamestate::stats
