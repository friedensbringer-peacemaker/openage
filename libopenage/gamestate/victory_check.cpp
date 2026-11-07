// Copyright 2026-2026 the openage authors. See copying.md for legal info.

/*
 * Host test of the victory rules (XR fork W8, gamestate/victory_math.h):
 *
 *   openage-victory-check
 *
 * Parameter sweeps:
 * - conquest: all unit classes 0-99 (exactly the classes of the spec list do
 *   not count), the building ids with their neighbours, the id range borders
 *   623/624/626/627 and 711/712/719/720, all finished/alive combinations,
 *   trebuchets, eliminated, conquest_won with and without teams, also with
 *   the player himself eliminated
 * - wonder: countdown table of all map sizes, all game modes with and without
 *   the one-time enemy extension, years rounding, the clock at the step sizes
 *   0.01/0.1/0.25/1/7 s (same ticks and the same countdown, every message
 *   exactly once, win exactly once, never below 0), wonder race wins with the
 *   first whole second
 * - relics: no relic on the map is no win, partial possession is no win,
 *   countdown start like the wonder (assumption L3), clock run to the win
 * Exit code 0 if all checks pass. No dependencies besides the standard library.
 */

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "gamestate/victory_math.h"

using namespace openage::gamestate;
using namespace openage::gamestate::victory;

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

/// all map sizes of the spec table
const std::array<map_size_t, 7> MAP_SIZES{
	map_size_t::TINY,
	map_size_t::SMALL,
	map_size_t::MEDIUM,
	map_size_t::NORMAL,
	map_size_t::LARGE,
	map_size_t::HUGE,
	map_size_t::GIGANTIC,
};

void check_conquest() {
	// finished and alive is the entry condition (spec 01 §1.5)
	check(counts_for_survival(4, 300, true, true), "a military class counts");
	check(not counts_for_survival(4, 300, false, true), "an unfinished object does not count");
	check(not counts_for_survival(4, 300, true, false), "a dead object does not count");
	check(not counts_for_survival(4, 300, false, false), "neither finished nor alive does not count");

	// classes that are not on the exclusion list count (villagers, monks,
	// military, free numbers of the sweep below)
	for (int cls : {0, 3, 4, 5, 6, 7, 8, 9, 10, 12}) {
		check(counts_for_survival(cls, 83, true, true), "class not on the list counts");
	}

	// sweep over all classes 0-99: exactly the classes of the spec list do not
	// count (the trebuchet classes 51 and 54 are not on it and count)
	for (int cls = 0; cls <= 99; cls++) {
		const bool expected = not(cls == 11 or cls == 30 or cls == 27 or cls == 52 or cls == 39
		                          or cls == 1 or cls == 60 or cls == 42 or cls == 21 or cls == 20
		                          or cls == 2 or cls == 19 or cls == 49 or cls == 37 or cls == 58
		                          or cls == 14);
		check(counts_for_survival(cls, 83, true, true) == expected,
		      "class sweep: only the spec list does not count");
	}
	check(counts_for_survival(51, 83, true, true), "trebuchet class 51 counts");
	check(counts_for_survival(54, 83, true, true), "trebuchet class 54 counts");

	// the building ids of the spec list do not count, their neighbours do
	for (int id : {70, 103, 209, 128, 598, 584, 68, 562}) {
		check(not counts_for_survival(4, id, true, true), "excluded building id does not count");
		check(counts_for_survival(4, id - 1, true, true), "the id before it counts");
		check(counts_for_survival(4, id + 1, true, true), "the id after it counts");
	}

	// the id range borders (spec 01 §1.5): 623 counts, 624-626 do not, 627
	// counts; 711 counts, 712-719 do not, 720 counts
	check(counts_for_survival(4, 623, true, true), "id 623 counts");
	for (int id = 624; id <= 626; id++) {
		check(not counts_for_survival(4, id, true, true), "ids 624-626 do not count");
	}
	check(counts_for_survival(4, 627, true, true), "id 627 counts");
	check(counts_for_survival(4, 711, true, true), "id 711 counts");
	for (int id = 712; id <= 719; id++) {
		check(not counts_for_survival(4, id, true, true), "ids 712-719 do not count");
	}
	check(counts_for_survival(4, 720, true, true), "id 720 counts");

	// eliminated: no counting object eliminates (spec 01 §1.5)
	check(eliminated(0), "no counting object eliminates");
	check(not eliminated(1), "one counting object keeps alive");
	check(not eliminated(7), "several counting objects keep alive");

	// conquest_won (spec 01 §1.5)
	check(conquest_won({{false, -1}}, 0), "a single living player has won");
	check(not conquest_won({{true, -1}}, 0), "a single eliminated player has not won");
	check(conquest_won({{false, -1}, {true, -1}}, 0), "eliminated opponent wins");
	check(not conquest_won({{false, -1}, {false, -1}}, 0), "living opponent blocks the win");
	check(not conquest_won({{true, -1}, {true, -1}}, 0), "eliminated player never wins");
	check(not conquest_won({{false, -1}}, 1), "an unknown player index is no win");
	check(not conquest_won({{false, -1}}, -1), "a negative player index is no win");

	// teams: only the players outside the own team matter
	const std::vector<PlayerState> won_teams{{false, 0}, {true, 0}, {true, 1}, {true, 1}};
	check(conquest_won(won_teams, 0), "all opponents eliminated wins, teammate irrelevant");
	check(not conquest_won({{false, 0}, {false, 0}, {false, 1}, {true, 1}}, 0),
	      "one living opponent blocks");
	check(conquest_won({{false, 0}, {false, 0}, {true, 1}, {true, 1}}, 0),
	      "both opponents eliminated wins");
	check(conquest_won({{true, 0}, {false, 0}, {true, 1}, {true, 1}}, 1),
	      "a player wins through his living teammate");
	check(not conquest_won({{true, 0}, {false, 0}, {false, 1}, {true, 1}}, 1),
	      "a living opponent blocks player 1");
	check(conquest_won({{false, 0}, {false, 0}}, 0), "a game with only teammates is won at once");
	check(conquest_won({{false, 0}, {true, 0}}, 0), "no opponent left beside a teammate wins");

	// team -1 = no team: every other player is an opponent
	check(not conquest_won({{false, -1}, {false, -1}}, 0), "two teamless players are opponents");
	check(conquest_won({{false, -1}, {true, -1}}, 0), "eliminated teamless opponent wins");
	check(conquest_won({{false, -1}, {true, 0}, {true, 0}}, 0),
	      "teamless player wins against eliminated teams");
	check(not conquest_won({{false, -1}, {true, 0}, {false, 0}}, 0),
	      "one living player of the team blocks the teamless player");
	check(not conquest_won({{false, -1}, {false, 0}, {false, 0}}, 1),
	      "a teamless player is an opponent of a team");
	check(conquest_won({{true, -1}, {false, 0}, {false, 0}}, 1),
	      "an eliminated teamless opponent lets the team win");
}

void check_wonder() {
	// countdown table (spec 01 §1.5): tenths of years by map size
	check(wonder_countdown_base(map_size_t::TINY) == 2000, "tiny 2000");
	check(wonder_countdown_base(map_size_t::SMALL) == 2000, "small 2000");
	check(wonder_countdown_base(map_size_t::MEDIUM) == 2000, "medium 2000");
	check(wonder_countdown_base(map_size_t::NORMAL) == 3000, "normal 3000");
	check(wonder_countdown_base(map_size_t::LARGE) == 3500, "large 3500");
	check(wonder_countdown_base(map_size_t::HUGE) == 4000, "huge 4000");
	check(wonder_countdown_base(map_size_t::GIGANTIC) == 5000, "gigantic 5000");

	// game modes (spec 01 §1.5): standard = base, defend +500, race 1; the
	// enemy extension (resource 196 of an opponent, "+100 years" = +1000) is
	// added once, the race adds nothing
	for (map_size_t size : MAP_SIZES) {
		check(wonder_countdown_start(size, wonder_mode_t::STANDARD, 0) == wonder_countdown_base(size),
		      "standard start is the base");
		check(wonder_countdown_start(size, wonder_mode_t::DEFEND, 0)
		          == wonder_countdown_base(size) + 500,
		      "defend adds 500");
		check(wonder_countdown_start(size, wonder_mode_t::RACE, 0) == 1, "race starts at 1");
		check(wonder_countdown_start(size, wonder_mode_t::RACE, 1000) == 1,
		      "race ignores the extension");
		check(wonder_countdown_start(size, wonder_mode_t::STANDARD, 1000)
		          == wonder_countdown_base(size) + 1000,
		      "standard adds the extension once");
		check(wonder_countdown_start(size, wonder_mode_t::DEFEND, 1000)
		          == wonder_countdown_base(size) + 500 + 1000,
		      "defend adds base, 500 and the extension");
	}

	// years for the message (spec 01 §1.2, §1.5): tenths of years / 10, up
	check(wonder_years(2000) == 200, "2000 tenths are 200 years");
	check(wonder_years(1999) == 200, "1999 tenths round up to 200 years");
	check(wonder_years(1991) == 200, "1991 tenths round up to 200 years");
	check(wonder_years(1990) == 199, "1990 tenths are 199 years");
	check(wonder_years(11) == 2, "11 tenths round up to 2 years");
	check(wonder_years(1) == 1, "1 tenth is 1 year");
	check(wonder_years(0) == 0, "0 tenths are 0 years");
}

/// run the clock for one step size and count the events with the elapsed time
/// at which each fired; used for the step size independence
struct ClockRun {
	int crossed_1000;
	int crossed_500;
	int wins;
	int countdown_at_win;
	int below_zero_steps;
	double elapsed_1000;
	double elapsed_500;
	double elapsed_win;
};

ClockRun run_clock(WonderClock &clock, double dt, int steps) {
	ClockRun run{0, 0, 0, -1, 0, -1.0, -1.0, -1.0};
	double elapsed = 0.0;
	for (int s = 0; s < steps; s++) {
		const WonderTick step = wonder_tick(clock, dt);
		elapsed += dt;
		if (step.crossed_1000) {
			run.crossed_1000 += 1;
			run.elapsed_1000 = elapsed;
		}
		if (step.crossed_500) {
			run.crossed_500 += 1;
			run.elapsed_500 = elapsed;
		}
		if (step.won) {
			run.wins += 1;
			run.countdown_at_win = step.countdown;
			run.elapsed_win = elapsed;
		}
		if (step.countdown < 0) {
			run.below_zero_steps += 1;
		}
	}
	return run;
}

void check_clock() {
	// one-shot ticks: only whole seconds tick (spec 01 §1.2, §1.5)
	WonderTick t = wonder_tick(2000, 0.25);
	check(t.countdown == 2000 and not t.crossed_1000 and not t.crossed_500 and not t.won,
	      "0.25 s does not tick");
	t = wonder_tick(2000, 0.99);
	check(t.countdown == 2000, "0.99 s does not tick");
	t = wonder_tick(2000, 1.0);
	check(t.countdown == 1998 and not t.won, "1 s ticks -2");
	t = wonder_tick(2000, 7.0);
	check(t.countdown == 1986, "7 s tick -14");
	t = wonder_tick(3, 1.0);
	check(t.countdown == 1 and not t.won, "an odd countdown ticks to 1");
	t = wonder_tick(1, 1.0);
	check(t.countdown == 0 and t.won, "an odd countdown clamps at 0 and wins");

	// thresholds: crossed exactly in the step that drops BELOW (spec 01 §1.5)
	t = wonder_tick(1002, 1.0);
	check(t.countdown == 1000 and not t.crossed_1000, "1000 exactly is not below 1000");
	t = wonder_tick(1000, 1.0);
	check(t.countdown == 998 and t.crossed_1000 and not t.crossed_500, "998 crosses 1000");
	t = wonder_tick(502, 1.0);
	check(t.countdown == 500 and not t.crossed_500, "500 exactly is not below 500");
	t = wonder_tick(500, 1.0);
	check(t.countdown == 498 and t.crossed_500 and not t.won, "498 crosses 500");
	t = wonder_tick(2, 1.0);
	check(t.countdown == 0 and t.won and not t.crossed_500, "0 wins without the 500 message");
	t = wonder_tick(0, 1.0);
	check(t.countdown == 0 and not t.won, "no second win at 0");
	t = wonder_tick(4, 7.0);
	check(t.countdown == 0 and t.won, "several seconds in one step win once");
	t = wonder_tick(1004, 7.0);
	check(t.countdown == 990 and t.crossed_1000, "a big step crosses 1000 once on the way");

	// after the win the clock stays at 0 without events
	WonderClock done{0, 0.0};
	for (int i = 0; i < 10; i++) {
		t = wonder_tick(done, 7.0);
		check(t.countdown == 0 and not t.won and not t.crossed_1000 and not t.crossed_500,
		      "after the win the clock stays at 0 without events");
	}

	// fractions carry over: 4 x 0.3 s = 1.2 s -> one tick, 0.2 s left
	WonderClock carry{2000, 0.0};
	wonder_tick(carry, 0.3);
	wonder_tick(carry, 0.3);
	wonder_tick(carry, 0.3);
	check(carry.countdown == 2000, "0.9 s is not a whole second");
	t = wonder_tick(carry, 0.3);
	check(t.countdown == 1998, "the fourth 0.3 s completes the second");
	check(near(carry.seconds_left, 0.2), "0.2 s carry over");

	// dt 0 does nothing
	WonderClock idle{2000, 0.5};
	t = wonder_tick(idle, 0.0);
	check(t.countdown == 2000 and near(idle.seconds_left, 0.5), "dt 0 does nothing");

	// the same run at the step sizes 0.01 / 0.1 / 0.25 / 1 / 7 s: 1010 s from
	// a 2000 countdown win, every message exactly once in the step of the
	// analytic second (501st tick crosses 1000 at 998, 751st crosses 500 at
	// 498, 1000th wins), never below 0
	for (double dt : {0.01, 0.1, 0.25, 1.0, 7.0}) {
		WonderClock clock{
			wonder_countdown_start(map_size_t::MEDIUM, wonder_mode_t::STANDARD, 0), 0.0};
		const int steps = static_cast<int>(1010.0 / dt) + 1;
		const ClockRun run = run_clock(clock, dt, steps);
		check(run.below_zero_steps == 0, "the countdown is never below 0");
		check(run.crossed_1000 == 1, "1000 crossed exactly once for every step size");
		check(run.elapsed_1000 >= 501.0 - 1e-9 and run.elapsed_1000 - dt < 501.0 + 1e-9,
		      "1000 crossed in the step of the 501st second for every step size");
		check(run.crossed_500 == 1, "500 crossed exactly once for every step size");
		check(run.elapsed_500 >= 751.0 - 1e-9 and run.elapsed_500 - dt < 751.0 + 1e-9,
		      "500 crossed in the step of the 751st second for every step size");
		check(run.wins == 1 and run.countdown_at_win == 0,
		      "win exactly once at 0 for every step size");
		check(run.elapsed_win >= 1000.0 - 1e-9 and run.elapsed_win - dt < 1000.0 + 1e-9,
		      "win in the step of the 1000th second for every step size");
		check(clock.countdown == 0, "final countdown 0 for every step size");
		check(clock.seconds_left >= 0.0 and clock.seconds_left < 1.0,
		      "the accumulator stays in [0, 1)");
	}

	// the same total time 504.5 s at every step size (floor of steps; the half
	// second keeps every step size safely BETWEEN two whole seconds, because
	// 0.01 and 0.1 are inexact in binary): the same whole seconds and the same
	// countdown
	for (double dt : {0.01, 0.1, 0.25, 1.0, 7.0}) {
		WonderClock clock{2000, 0.0};
		const int steps = static_cast<int>(504.5 / dt);
		run_clock(clock, dt, steps);
		check(clock.countdown == 2000 - 2 * 504,
		      "the same total time gives the same countdown for every step size");
		check(clock.seconds_left >= 0.0 and clock.seconds_left < 1.0 + 1e-9,
		      "the accumulator stays below one second");
	}

	// wonder race (spec 01 §1.5): countdown 1, the win comes with the first
	// whole second, exactly once, without a message
	for (double dt : {0.01, 0.1, 0.25, 1.0, 7.0}) {
		WonderClock clock{
			wonder_countdown_start(map_size_t::GIGANTIC, wonder_mode_t::RACE, 5000), 0.0};
		check(clock.countdown == 1, "race starts at 1 despite size and extension");
		int wins = 0;
		int messages = 0;
		double win_time = -1.0;
		double elapsed = 0.0;
		while (elapsed < 2.0) {
			const WonderTick step = wonder_tick(clock, dt);
			elapsed += dt;
			if (step.won) {
				wins += 1;
				win_time = elapsed;
			}
			if (step.crossed_1000 or step.crossed_500) {
				messages += 1;
			}
		}
		check(wins == 1, "race wins exactly once for every step size");
		check(win_time >= 1.0 - 1e-9 and win_time <= 1.0 + dt + 1e-9,
		      "race wins with the first whole second");
		check(messages == 0, "race sends no threshold message");
	}
}

void check_relics() {
	// all relics held (spec 01 §1.5): only with at least one relic on the map
	// and full possession
	check(not all_relics_held(0, 0), "no relic on the map is no relic win");
	check(not all_relics_held(5, 0), "relics held without relics on the map is no win");
	check(not all_relics_held(0, 5), "no relic held is no win");
	check(not all_relics_held(4, 5), "partial possession is no win");
	check(not all_relics_held(1, 5), "one of five relics is no win");
	check(all_relics_held(5, 5), "all five relics held wins");
	check(all_relics_held(6, 5), "more held than on the map counts as all");

	// sweep: only held >= on_map with on_map > 0 wins
	for (int on_map = 0; on_map <= 8; on_map++) {
		for (int held = 0; held <= 9; held++) {
			check(all_relics_held(held, on_map) == (on_map > 0 and held >= on_map),
			      "relic sweep: only full possession wins");
		}
	}

	// countdown start: ASSUMPTION L3 (docs/aoe2-spec/LUECKEN.md), the same
	// table as the wonder; the clock is the same one
	for (map_size_t size : MAP_SIZES) {
		check(relic_countdown_start(size) == wonder_countdown_base(size),
		      "relic countdown like the wonder (assumption L3)");
	}
	WonderClock clock{relic_countdown_start(map_size_t::TINY), 0.0};
	const ClockRun run = run_clock(clock, 1.0, 1100);
	check(run.wins == 1 and run.countdown_at_win == 0, "relic clock wins once at 0");
	check(run.crossed_1000 == 1 and run.crossed_500 == 1, "both relic messages on the way");
	check(clock.countdown == 0, "relic countdown reaches 0 after 1000 seconds");
}

} // namespace

int main() {
	check_conquest();
	check_wonder();
	check_clock();
	check_relics();

	std::cout << "victory check: " << checks << " checks, " << failures << " failures" << std::endl;
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
