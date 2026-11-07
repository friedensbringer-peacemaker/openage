// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the score rules (XR fork W8, gamestate/score_math.h):
 *
 *   openage-score-check
 *
 * Hand computed examples: every part of the score with whole and broken
 * values, the rounding down of each part, the total as the sum of the
 * rounded parts (spec 13 L4 example 0/284/50/0 = 334) and the leader
 * with the highest total (tie keeps the first, empty list).
 * Exit code 0 if all checks pass. No dependencies besides the standard library.
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "gamestate/score_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::score;

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

ScoreInput input(double destroyed, double resources, double tribute, double remaining,
                 double researched, double explored, double castle_wonder) {
	return ScoreInput{destroyed, resources, tribute, remaining, researched, explored,
	                  castle_wonder};
}

void check_parts() {
	// military: 20% of the destroyed and converted value
	check(compute(input(0, 0, 0, 0, 0, 0, 0)).total == 0, "all zero scores zero");
	check(compute(input(100, 0, 0, 0, 0, 0, 0)).military == 20, "military 20% of 100");
	check(compute(input(99.9, 0, 0, 0, 0, 0, 0)).military == 19, "military rounds down");

	// economy: 10% of resources plus tribute, 20% of the remaining value
	check(compute(input(0, 500, 100, 1000, 0, 0, 0)).economy == 260, "economy 60 + 200");
	check(compute(input(0, 515, 0, 0, 0, 0, 0)).economy == 51, "economy rounds down");
	check(compute(input(0, 0, 25, 0, 0, 0, 0)).economy == 2, "tribute counts");

	// technology: 20% of researched techs plus 10 per percent explored
	check(compute(input(0, 0, 0, 0, 250, 5, 0)).technology == 100, "technology 50 + 50");
	check(compute(input(0, 0, 0, 0, 9, 2.5, 0)).technology == 26, "technology rounds down");

	// society: 20% of the castle and wonder cost
	check(compute(input(0, 0, 0, 0, 0, 0, 2500)).society == 500, "society 20% of 2500");
	check(compute(input(0, 0, 0, 0, 0, 0, 2499)).society == 499, "society rounds down");
}

void check_total() {
	// every part has a fraction, each part is rounded on its own
	const Score s = compute(input(107, 515, 25, 45, 9, 2.5, 2499));
	check(s.military == 21, "military 21.4 -> 21");
	check(s.economy == 63, "economy 54 + 9 -> 63");
	check(s.technology == 26, "technology 1.8 + 25 -> 26");
	check(s.society == 499, "society 499.8 -> 499");
	check(s.total == s.military + s.economy + s.technology + s.society, "total is the sum");
	check(s.total == 609, "total 609");

	// spec 13 L4 example: 0 / 284 / 50 / 0 = 334
	const Score e = compute(input(0, 2840, 0, 0, 0, 5, 0));
	check(e.military == 0, "example military 0");
	check(e.economy == 284, "example economy 284");
	check(e.technology == 50, "example technology 50");
	check(e.society == 0, "example society 0");
	check(e.total == 334, "example total 334");
}

void check_leader() {
	check(leader({}) == -1, "empty list has no leader");
	const std::vector<Score> one = {Score{10, 0, 0, 0, 10}};
	check(leader(one) == 0, "one player leads");
	const std::vector<Score> three = {Score{5, 5, 5, 5, 20}, Score{100, 0, 0, 0, 100},
	                                  Score{30, 30, 30, 30, 120}};
	check(leader(three) == 2, "highest total leads");
	const std::vector<Score> tie = {Score{9, 9, 9, 9, 36}, Score{36, 0, 0, 0, 36}};
	check(leader(tie) == 0, "tie keeps the first");
}

} // namespace

int main() {
	check_parts();
	check_total();
	check_leader();
	std::cout << "score check: " << checks << " checks, " << failures << " failures"
	          << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
