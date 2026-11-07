// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>


namespace openage::gamestate::victory {

/**
 * Rules of the conquest defeat, the wonder countdown and the relics
 * (XR fork W8, docs/aoe2-spec/01-spielablauf.md §1.5, clock §1.2): which
 * objects keep a player alive, when a player is eliminated and the conquest
 * is won, how the wonder countdown in tenths of years starts and ticks and
 * when a team holds all relics. Written from the specification; every number
 * below is one the specification names.
 *
 * This header has no engine dependencies (host test gamestate/victory_check.cpp).
 */

// ----------------------------------------------------------------- conquest

/// unit classes that never keep a player alive (spec 01 §1.5): miscellaneous,
/// flags, walls, towers, gates, artifacts, class 60, relics, fishing ships,
/// transport ships, trade ships, trade carts, farms, doubles, herdables and
/// terrain
constexpr std::array<int, 16> NON_COUNTING_CLASSES{
	11,  // miscellaneous
	30,  // flags
	27,  // walls
	52,  // towers
	39,  // gates
	1,   // artifacts
	60,  // unnamed in the specification
	42,  // relics
	21,  // fishing ships
	20,  // transport ships
	2,   // trade ships
	19,  // trade carts
	49,  // farms
	37,  // doubles
	58,  // herdables
	14,  // terrain
};

/// building ids that never keep a player alive (spec 01 §1.5)
constexpr std::array<int, 8> NON_COUNTING_BUILDING_IDS{
	70,   // house
	103,  // blacksmith
	209,  // university
	128,  // trade cart (named as a building id in the spec)
	598,  // outpost
	584,  // mining camp
	68,   // mill
	562,  // lumber camp
};

/// id range of age and special building variants (spec 01 §1.5): 624-626
constexpr int NON_COUNTING_ID_RANGE_A_FIRST = 624;
constexpr int NON_COUNTING_ID_RANGE_A_LAST = 626;
/// second id range of age and special building variants (spec 01 §1.5): 712-719
constexpr int NON_COUNTING_ID_RANGE_B_FIRST = 712;
constexpr int NON_COUNTING_ID_RANGE_B_LAST = 719;

/// trebuchet classes (spec 01 §1.5): they count in the packed AND in the
/// unpacked state
constexpr std::array<int, 2> TREBUCHET_CLASSES{
	51,
	54,
};

/// is the value one of the list?
template <std::size_t N>
constexpr bool in_list(const std::array<int, N> &list, int value) {
	for (const int entry : list) {
		if (entry == value) {
			return true;
		}
	}
	return false;
}

/// does one object keep its owner alive (spec 01 §1.5)? An object counts when
/// it is finished and alive and belongs to none of the excluded groups; the
/// trebuchet classes count in the packed and unpacked state
inline bool counts_for_survival(int unit_class, int unit_id, bool finished, bool alive) {
	if (not finished or not alive) {
		return false;
	}
	if (in_list(TREBUCHET_CLASSES, unit_class)) {
		return true;
	}
	if (in_list(NON_COUNTING_CLASSES, unit_class)) {
		return false;
	}
	if (in_list(NON_COUNTING_BUILDING_IDS, unit_id)) {
		return false;
	}
	if (unit_id >= NON_COUNTING_ID_RANGE_A_FIRST and unit_id <= NON_COUNTING_ID_RANGE_A_LAST) {
		return false;
	}
	if (unit_id >= NON_COUNTING_ID_RANGE_B_FIRST and unit_id <= NON_COUNTING_ID_RANGE_B_LAST) {
		return false;
	}
	return true;
}

/// is a player eliminated (spec 01 §1.5)? Yes when the per-second check finds
/// no counting object of him
constexpr bool eliminated(int counting_objects) {
	return counting_objects <= 0;
}

/// state of one player for the conquest check
struct PlayerState {
	/// the player found no counting object
	bool eliminated;
	/// team number, -1 = no team
	int team;
};

/// has the player won the conquest (spec 01 §1.5)? Yes when he is not
/// eliminated and every player OUTSIDE his own team is; team -1 means no team,
/// so every other player is an opponent
inline bool conquest_won(const std::vector<PlayerState> &players, int player) {
	if (player < 0 or player >= static_cast<int>(players.size())) {
		return false;
	}
	const PlayerState &me = players[static_cast<std::size_t>(player)];
	if (me.eliminated) {
		return false;
	}
	for (std::size_t i = 0; i < players.size(); i++) {
		if (static_cast<int>(i) == player) {
			continue;
		}
		const bool allied = me.team >= 0 and players[i].team == me.team;
		if (not allied and not players[i].eliminated) {
			return false;
		}
	}
	return true;
}

// ------------------------------------------------------------------- wonder

/// map sizes (spec 01 §1.5)
enum class map_size_t {
	TINY,
	SMALL,
	MEDIUM,
	NORMAL,
	LARGE,
	HUGE,
	GIGANTIC,
};

/// base countdown of a finished wonder in tenths of years (spec 01 §1.5):
/// tiny/small/medium 2000 (= 200 years), normal 3000, large 3500, huge 4000,
/// gigantic 5000
constexpr int wonder_countdown_base(map_size_t size) {
	switch (size) {
	case map_size_t::TINY:
	case map_size_t::SMALL:
	case map_size_t::MEDIUM:
		return 2000;
	case map_size_t::NORMAL:
		return 3000;
	case map_size_t::LARGE:
		return 3500;
	case map_size_t::HUGE:
		return 4000;
	case map_size_t::GIGANTIC:
		return 5000;
	}
	return 0;
}

/// wonder game modes (spec 01 §1.5)
enum class wonder_mode_t {
	STANDARD,
	DEFEND,
	RACE,
};

/// start value of the wonder countdown in tenths of years (spec 01 §1.5): the
/// base by map size, "defend the wonder" +500, "wonder race" countdown 1 (an
/// instant win when the wonder finishes). enemy_extension is player resource
/// 196 of an opponent (e.g. hunnic atheism, "+100 years" = +1000); the spec
/// adds it ONCE at the start, the race adds nothing
constexpr int wonder_countdown_start(map_size_t size, wonder_mode_t mode, int enemy_extension) {
	if (mode == wonder_mode_t::RACE) {
		return 1;
	}
	int countdown = wonder_countdown_base(size);
	if (mode == wonder_mode_t::DEFEND) {
		countdown += 500;
	}
	return countdown + enemy_extension;
}

/// clock of the wonder and relic countdown (spec 01 §1.2, §1.5): the countdown
/// runs in tenths of years and drops by 2 per whole game second (1 year =
/// 5 game seconds), never below 0. Only whole seconds tick, so the frame step
/// size never changes the result; seconds_left is the time that has already
/// passed toward the next whole second (always in [0, 1))
struct WonderClock {
	/// remaining tenths of years
	int countdown;
	/// elapsed seconds toward the next whole second, in [0, 1)
	double seconds_left;
};

/// what one step of the clock did (spec 01 §1.5): the new countdown, the two
/// messages ("crossed" is true in exactly the step in which the countdown
/// drops BELOW the threshold) and the win at 0
struct WonderTick {
	/// countdown after the step
	int countdown;
	/// countdown dropped below 1000 (100 years) in this step
	bool crossed_1000;
	/// countdown dropped below 500 (50 years) in this step
	bool crossed_500;
	/// countdown reached 0 in this step (true in exactly this one step)
	bool won;
};

/// one whole second of the countdown: -2, never below 0, with the messages of
/// the second in which the countdown drops below 1000 and 500 and the win at 0
inline void wonder_second(WonderClock &clock, WonderTick &step) {
	const int old_countdown = clock.countdown;
	clock.countdown = std::max(0, clock.countdown - 2);
	if (old_countdown >= 1000 and clock.countdown < 1000) {
		step.crossed_1000 = true;
	}
	if (old_countdown >= 500 and clock.countdown < 500) {
		step.crossed_500 = true;
	}
	if (old_countdown > 0 and clock.countdown == 0) {
		step.won = true;
	}
}

/// advance the clock by dt seconds and report what happened (spec 01 §1.5):
/// whole seconds tick (each -2), leftover fractions carry over to the next
/// step, so every step size produces the same ticks
inline WonderTick wonder_tick(WonderClock &clock, double dt) {
	WonderTick step{clock.countdown, false, false, false};
	clock.seconds_left += dt;
	while (clock.seconds_left >= 1.0) {
		clock.seconds_left -= 1.0;
		wonder_second(clock, step);
	}
	step.countdown = clock.countdown;
	return step;
}

/// one-shot variant: a fresh clock with countdown, advanced by dt seconds
inline WonderTick wonder_tick(int countdown, double dt) {
	WonderClock clock{countdown, 0.0};
	return wonder_tick(clock, dt);
}

/// years for the message of a countdown in tenths of years (spec 01 §1.2,
/// §1.5): tenths of years / 10, rounded up (2000 -> 200, 1999 -> 200, 1 -> 1)
constexpr int wonder_years(int countdown) {
	return (countdown + 9) / 10;
}

// ------------------------------------------------------------------ relics

/// does one team hold all relics of the map (spec 01 §1.5)? True only when at
/// least one relic is on the map and the team holds all of them (held_by_team
/// counts the relics of the team, in monasteries or with a monk)
constexpr bool all_relics_held(int held_by_team, int relics_on_map) {
	return relics_on_map > 0 and held_by_team >= relics_on_map;
}

/// start value of the relic countdown in tenths of years: ASSUMPTION (gap L3
/// of docs/aoe2-spec/LUECKEN.md): the length was not found statically and is
/// presumed the same as the wonder countdown by map size; the countdown runs
/// with the same WonderClock as the wonder
constexpr int relic_countdown_start(map_size_t size) {
	return wonder_countdown_base(size);
}

} // namespace openage::gamestate::victory
