// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the AI difficulty and game speed rules (XR fork W9,
 * gamestate/handicap_math.h): openage-handicap-check
 *
 * Every level with and without optional factors, every game speed, years
 * of 0/5/1500 game seconds (1500 s = 300 years). Exit code 0 if all checks
 * pass. No dependencies besides the standard library.
 */

#include <cstdlib>
#include <iostream>
#include <string>

#include "gamestate/handicap_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::handicap;

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

void check_rates() {
	// without optional factors only EASY and EASIEST get a handicap
	check(production_rate(ai_level_t::HARDEST, false) == 1.0, "hardest without factors 1.0");
	check(production_rate(ai_level_t::HARD, false) == 1.0, "hard without factors 1.0");
	check(production_rate(ai_level_t::MODERATE, false) == 1.0, "moderate without factors 1.0");
	check(production_rate(ai_level_t::EASY, false) == 0.75, "easy 0.75");
	check(production_rate(ai_level_t::EASIEST, false) == 0.5, "easiest 0.5");
	// with optional factors also HARD and MODERATE
	check(production_rate(ai_level_t::HARDEST, true) == 1.0, "hardest with factors 1.0");
	check(production_rate(ai_level_t::HARD, true) == 0.95, "hard with factors 0.95");
	check(production_rate(ai_level_t::MODERATE, true) == 0.875, "moderate with factors 0.875");
	check(production_rate(ai_level_t::EASY, true) == 0.75, "easy with factors 0.75");
	check(production_rate(ai_level_t::EASIEST, true) == 0.5, "easiest with factors 0.5");
	check(human_rate() == 1.0, "human rate 1.0");
}

void check_speed_and_years() {
	check(game_seconds_per_real_second(game_speed_t::SLOW) == 1.0, "slow 1.0");
	check(game_seconds_per_real_second(game_speed_t::NORMAL) == 1.5, "normal 1.5");
	check(game_seconds_per_real_second(game_speed_t::FAST) == 2.0, "fast 2.0");
	check(GAME_SECONDS_PER_YEAR == 5.0, "five game seconds per year");
	check(years_of_game_seconds(0) == 0, "0 s is 0 years");
	check(years_of_game_seconds(5) == 1, "5 s is 1 year");
	check(years_of_game_seconds(1500) == 300, "1500 s are 300 years");
}

} // namespace

int main() {
	check_rates();
	check_speed_and_years();
	std::cout << "handicap check: " << checks << " checks, " << failures << " failures"
	          << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
