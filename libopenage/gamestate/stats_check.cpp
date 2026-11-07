// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the end of game columns (XR fork W8, gamestate/stats_math.h):
 * openage-stats-check. The star of a column, the population shares with
 * their sum and negative counts, the game clock under and from one hour.
 * Exit code 0 if all checks pass. Only the standard library.
 */

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "gamestate/stats_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::stats;

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

void check_column() {
	check(best_in_column({1.0, 3.0, 3.0, 0.0})
	          == std::vector<bool>{false, true, true, false},
	      "every best player gets the star");
	check(best_in_column({0.0, 0.0}) == std::vector<bool>{false, false},
	      "only zeros: nobody gets the star");
	check(best_in_column({-5.0, -1.0, -1.0}) == std::vector<bool>{false, false, false},
	      "best value below 0: nobody");
	check(best_in_column({2.5}) == std::vector<bool>{true}, "single player gets the star");
	check(best_in_column({}).empty(), "empty column has no stars");
}

void check_shares() {
	check(population_shares({10, 30}) == std::vector<double>{25.0, 75.0},
	      "shares in percent");
	check(population_shares({1, 2, 1}) == std::vector<double>{25.0, 50.0, 25.0},
	      "shares add up to 100");
	double sum = 0.0;
	for (const double share : population_shares({1, 1, 1})) {
		sum += share;
	}
	check(std::abs(sum - 100.0) < 1e-9, "thirds sum up to 100");
	check(population_shares({0, 0, 0}) == std::vector<double>{0.0, 0.0, 0.0},
	      "all zero gives only zeros");
	check(population_shares({-10, 30}) == std::vector<double>{0.0, 100.0},
	      "negative counts as 0");
	check(population_shares({4}) == std::vector<double>{100.0}, "single player 100 percent");
	check(population_shares({}).empty(), "no players, no shares");
}

void check_clock() {
	check(clock_text(0.0) == "0:00", "zero");
	check(clock_text(5.0) == "0:05", "seconds with leading zero");
	check(clock_text(65.0) == "1:05", "one minute five");
	check(clock_text(59.9) == "0:59", "rounded down to whole seconds");
	check(clock_text(3599.0) == "59:59", "just under one hour");
	check(clock_text(3600.0) == "1:00:00", "exactly one hour");
	check(clock_text(3661.5) == "1:01:01", "one hour one minute one second");
	check(clock_text(7323.0) == "2:02:03", "two hours");
	check(clock_text(-3.0) == "0:00", "negative counts as 0");
}

} // namespace

int main() {
	check_column();
	check_shares();
	check_clock();
	std::cout << "stats check: " << checks << " checks, " << failures << " failures"
	          << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
