// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>


/*
 * Decision rules of the computer opponent (XR fork, gamestate/ai).
 *
 * Dependency-free (only the standard library) so that the host test
 * openage-ai-rules-check can sweep the parameters without the engine.
 * The game state side lives in ai_player.h.
 */
namespace openage::gamestate::ai {

/// difficulty (same order as gamestate::ai_difficulty_t)
enum class difficulty_t {
	EASY,
	NORMAL,
};

inline const char *to_string(difficulty_t difficulty) {
	return difficulty == difficulty_t::EASY ? "easy" : "normal";
}

/// resources the villagers of the AI gather (stone is not used by anything yet)
enum class gather_t : size_t {
	FOOD,
	WOOD,
	GOLD,
	COUNT,
};

constexpr size_t GATHER_KINDS = static_cast<size_t>(gather_t::COUNT);

inline const char *to_string(gather_t kind) {
	switch (kind) {
	case gather_t::FOOD:
		return "food";
	case gather_t::WOOD:
		return "wood";
	case gather_t::GOLD:
		return "gold";
	default:
		return "?";
	}
}

/**
 * Parameters of the computer opponent. params_for() fills them per difficulty,
 * MapSettings::ai may override the attack values.
 */
struct AiParams {
	difficulty_t difficulty = difficulty_t::EASY;
	/// reaction time: period of the decisions (simulation seconds)
	double think_period = 2.0;
	/// period of the status report in the log (simulation seconds)
	double report_period = 30.0;
	/// share of the villagers per resource (food, wood, gold), sum 1
	std::array<double, GATHER_KINDS> gather_split{0.4, 0.4, 0.2};
	/// a villager that got an order is left alone for this long (s)
	double order_cooldown = 3.0;

	/// army size at home that starts an attack wave
	size_t attack_threshold = 6;
	/// no attack before this simulation time (s) - the human builds up meanwhile
	double first_attack_earliest = 480.0;
	/// after this time the AI attacks with any army (s)
	double attack_anyway_after = 720.0;
	/// minimum time between two waves (s)
	double wave_interval = 120.0;
	/// enemies closer than this to an own building are a threat (tiles)
	double defense_radius = 10.0;
	/// enemies closer than this to an own villager are a threat (tiles)
	double villager_guard_radius = 5.0;
	/// wave units switch from buildings to enemy units within line of sight + this (tiles)
	double retarget_extra = 2.0;
	/// an army unit gets a new attack order at most this often (s)
	double unit_order_cooldown = 4.0;

	/// villagers to train
	size_t villager_target = 15;
	/// build a house when the population is this close to the limit
	size_t house_margin = 2;
	/// build a barracks once this many villagers exist
	size_t barracks_after_villagers = 8;
	/// militia queued at once (per barracks)
	size_t max_queued_military = 2;
	/// villagers queued at once (per town center)
	size_t max_queued_villagers = 2;
};

/**
 * Parameters of a difficulty.
 *
 * EASY: reaction 2 s, first attack at 8 min at the earliest with 6 units
 *       (any army after 12 min), waves every 2 min, defence radius 10 tiles.
 * NORMAL: reaction 1 s, first attack at 4 min with 5 units (any army after
 *       8 min), waves every minute, defence radius 14 tiles.
 */
inline AiParams params_for(difficulty_t difficulty) {
	AiParams p;
	p.difficulty = difficulty;
	if (difficulty == difficulty_t::NORMAL) {
		p.think_period = 1.0;
		p.attack_threshold = 5;
		p.first_attack_earliest = 240.0;
		p.attack_anyway_after = 480.0;
		p.wave_interval = 60.0;
		p.defense_radius = 14.0;
		p.villager_guard_radius = 6.0;
		p.unit_order_cooldown = 2.0;
	}
	return p;
}

/**
 * Resource for an idle villager: the one with the largest shortfall against
 * the split (target share of all workers including the new one). Kinds
 * without reachable resources are skipped; ties go to the lower kind.
 *
 * @param workers Villagers per kind that already gather.
 * @param split Target share per kind.
 * @param available Kinds that still have resources on the map.
 *
 * @return Kind, or nothing if no kind is available.
 */
inline std::optional<gather_t> choose_gather(const std::array<size_t, GATHER_KINDS> &workers,
                                             const std::array<double, GATHER_KINDS> &split,
                                             const std::array<bool, GATHER_KINDS> &available) {
	size_t total = 1;
	for (auto w : workers) {
		total += w;
	}
	std::optional<gather_t> best;
	double best_deficit = -1e30;
	for (size_t k = 0; k < GATHER_KINDS; ++k) {
		if (not available[k]) {
			continue;
		}
		double deficit = split[k] * static_cast<double>(total) - static_cast<double>(workers[k]);
		if (deficit > best_deficit + 1e-9) {
			best_deficit = deficit;
			best = static_cast<gather_t>(k);
		}
	}
	return best;
}

/// why the AI attacks or waits
enum class attack_reason_t {
	/// no army
	NO_ARMY,
	/// before AiParams::first_attack_earliest
	TOO_EARLY,
	/// the last wave was less than AiParams::wave_interval ago
	COOLDOWN,
	/// fewer units than the threshold
	TOO_FEW,
	/// army reached the threshold
	THRESHOLD,
	/// waited long enough, attack with what is there
	TIMEOUT,
};

inline const char *to_string(attack_reason_t reason) {
	switch (reason) {
	case attack_reason_t::NO_ARMY:
		return "no army";
	case attack_reason_t::TOO_EARLY:
		return "too early";
	case attack_reason_t::COOLDOWN:
		return "cooldown";
	case attack_reason_t::TOO_FEW:
		return "too few units";
	case attack_reason_t::THRESHOLD:
		return "threshold reached";
	case attack_reason_t::TIMEOUT:
		return "waited long enough";
	default:
		return "?";
	}
}

struct AttackDecision {
	bool attack = false;
	attack_reason_t reason = attack_reason_t::NO_ARMY;
};

/**
 * Start a new attack wave?
 *
 * @param now Simulation time (s).
 * @param army Military units at home (not in a wave, not defending).
 * @param waves Waves so far.
 * @param last_wave Start time of the last wave (s, ignored without waves).
 * @param p Parameters.
 */
inline AttackDecision attack_decision(double now, size_t army, size_t waves, double last_wave, const AiParams &p) {
	if (army == 0) {
		return {false, attack_reason_t::NO_ARMY};
	}
	if (now < p.first_attack_earliest) {
		return {false, attack_reason_t::TOO_EARLY};
	}
	if (waves > 0 and now - last_wave < p.wave_interval) {
		return {false, attack_reason_t::COOLDOWN};
	}
	if (army >= p.attack_threshold) {
		return {true, attack_reason_t::THRESHOLD};
	}
	bool waited = waves == 0 ? now >= p.attack_anyway_after
	                         : now - last_wave >= 2.0 * p.wave_interval;
	if (waited) {
		return {true, attack_reason_t::TIMEOUT};
	}
	return {false, attack_reason_t::TOO_FEW};
}

/// earliest time of the next wave (for the report), given the current army
inline double next_attack_time(double now, size_t army, size_t waves, double last_wave, const AiParams &p) {
	double t = std::max(now, p.first_attack_earliest);
	if (waves > 0) {
		t = std::max(t, last_wave + p.wave_interval);
	}
	if (army < p.attack_threshold) {
		double timeout = waves == 0 ? p.attack_anyway_after : last_wave + 2.0 * p.wave_interval;
		t = std::max(t, timeout);
	}
	return t;
}

/// an enemy entity the AI may attack
struct TargetCandidate {
	uint64_t id = 0;
	/// distance to the reference point (army centre, unit) in tiles
	double distance = 0.0;
	bool building = false;
	bool town_center = false;
	/// can fight back (not a villager)
	bool military = false;
};

/**
 * Objective of an attack wave: the nearest enemy town center, else the
 * nearest enemy (units before buildings at equal distance); ties by id.
 *
 * @return Index into \p candidates.
 */
inline std::optional<size_t> choose_objective(const std::vector<TargetCandidate> &candidates) {
	std::optional<size_t> best;
	auto better = [&](const TargetCandidate &a, const TargetCandidate &b) {
		if (a.town_center != b.town_center) {
			return a.town_center;
		}
		if (std::abs(a.distance - b.distance) > 1e-9) {
			return a.distance < b.distance;
		}
		if (a.building != b.building) {
			return not a.building;
		}
		return a.id < b.id;
	};
	for (size_t i = 0; i < candidates.size(); ++i) {
		if (not best or better(candidates[i], candidates[*best])) {
			best = i;
		}
	}
	return best;
}

/**
 * Target of a single unit of a wave or of the defence: the nearest enemy
 * unit within \p reach (military before villagers: +2 tiles penalty),
 * buildings are left to the objective.
 *
 * @return Index into \p candidates.
 */
inline std::optional<size_t> choose_unit_target(const std::vector<TargetCandidate> &candidates, double reach) {
	std::optional<size_t> best;
	double best_score = 1e30;
	for (size_t i = 0; i < candidates.size(); ++i) {
		const auto &c = candidates[i];
		if (c.building or c.distance > reach) {
			continue;
		}
		double score = c.distance + (c.military ? 0.0 : 2.0);
		if (score < best_score - 1e-9 or (std::abs(score - best_score) <= 1e-9 and best and c.id < candidates[*best].id)) {
			best_score = score;
			best = i;
		}
	}
	return best;
}

/// population limit of finished town centers and houses (AoE II: +5 each, at most 200)
inline size_t estimated_population_cap(size_t town_centers, size_t houses) {
	return std::min<size_t>(200, 5 * (town_centers + houses));
}

/// what the AI wants to produce
enum class plan_t {
	TRAIN_VILLAGER,
	BUILD_HOUSE,
	BUILD_BARRACKS,
	TRAIN_MILITIA,
};

inline const char *to_string(plan_t plan) {
	switch (plan) {
	case plan_t::TRAIN_VILLAGER:
		return "train villager";
	case plan_t::BUILD_HOUSE:
		return "build house";
	case plan_t::BUILD_BARRACKS:
		return "build barracks";
	case plan_t::TRAIN_MILITIA:
		return "train militia";
	default:
		return "?";
	}
}

/// AoE II costs (food, wood, gold, stone); the real costs come from nyan once production exists
constexpr std::array<double, 4> COST_VILLAGER{50.0, 0.0, 0.0, 0.0};
constexpr std::array<double, 4> COST_MILITIA{60.0, 0.0, 20.0, 0.0};
constexpr std::array<double, 4> COST_HOUSE{0.0, 25.0, 0.0, 0.0};
constexpr std::array<double, 4> COST_BARRACKS{0.0, 175.0, 0.0, 0.0};

inline bool affordable(const std::array<double, 4> &stock, const std::array<double, 4> &cost) {
	for (size_t i = 0; i < stock.size(); ++i) {
		if (stock[i] + 1e-9 < cost[i]) {
			return false;
		}
	}
	return true;
}

/// economy of the AI for the production plan
struct EconomyView {
	size_t villagers = 0;
	size_t military = 0;
	size_t queued_villagers = 0;
	size_t queued_military = 0;
	size_t town_centers = 0;
	size_t houses = 0;
	size_t barracks = 0;
	/// foundations not finished yet
	size_t houses_planned = 0;
	size_t barracks_planned = 0;
	size_t population = 0;
	size_t population_cap = 0;
	/// food, wood, gold, stone
	std::array<double, 4> stock{};
};

/**
 * Production wishes in priority order (house, villager, barracks, militia).
 * Resources are reserved in that order, so a later wish is only listed if it
 * is affordable after the earlier ones.
 */
inline std::vector<plan_t> plan_production(const EconomyView &e, const AiParams &p) {
	std::vector<plan_t> plan;
	auto stock = e.stock;
	auto take = [&](const std::array<double, 4> &cost) {
		if (not affordable(stock, cost)) {
			return false;
		}
		for (size_t i = 0; i < stock.size(); ++i) {
			stock[i] -= cost[i];
		}
		return true;
	};
	bool housing_short = e.population_cap < 200 and e.population + p.house_margin >= e.population_cap;
	if (housing_short and e.houses_planned == 0 and e.villagers > 0 and take(COST_HOUSE)) {
		plan.push_back(plan_t::BUILD_HOUSE);
	}
	bool room = e.population < e.population_cap;
	if (e.town_centers > 0 and room and e.villagers + e.queued_villagers < p.villager_target
	    and e.queued_villagers < p.max_queued_villagers * e.town_centers and take(COST_VILLAGER)) {
		plan.push_back(plan_t::TRAIN_VILLAGER);
	}
	if (e.barracks == 0 and e.barracks_planned == 0 and e.villagers >= p.barracks_after_villagers
	    and take(COST_BARRACKS)) {
		plan.push_back(plan_t::BUILD_BARRACKS);
	}
	if (e.barracks > 0 and room and e.queued_military < p.max_queued_military * e.barracks
	    and take(COST_MILITIA)) {
		plan.push_back(plan_t::TRAIN_MILITIA);
	}
	return plan;
}

/**
 * Deterministic random numbers (SplitMix64) for tie breaks; same seed = same game.
 */
class Rng {
public:
	explicit Rng(uint64_t seed) :
		state{seed} {}

	uint64_t next() {
		uint64_t z = (this->state += 0x9e3779b97f4a7c15ULL);
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		return z ^ (z >> 31);
	}

	/// uniform in [0, 1)
	double uniform() {
		return static_cast<double>(this->next() >> 11) * (1.0 / 9007199254740992.0);
	}

private:
	uint64_t state;
};

/**
 * Candidate spots for a building around a centre (town center), ring by ring
 * from \p r_min to \p r_max tiles, 12 directions per ring starting at a seeded
 * angle. Spots are tile centres (x.5) for odd sides, tile corners otherwise.
 */
inline std::vector<std::pair<double, double>> building_spots(double cx, double cy, double r_min, double r_max,
                                                             double side, Rng &rng) {
	std::vector<std::pair<double, double>> spots;
	const double start = rng.uniform() * 6.283185307179586;
	const double offset = (static_cast<long>(std::lround(side)) % 2 == 1) ? 0.5 : 0.0;
	for (double r = r_min; r <= r_max + 1e-9; r += 1.0) {
		for (int k = 0; k < 12; ++k) {
			double a = start + k * 0.5235987755982988;
			double x = std::floor(cx + r * std::cos(a)) + offset;
			double y = std::floor(cy + r * std::sin(a)) + offset;
			if (std::find(spots.begin(), spots.end(), std::pair{x, y}) == spots.end()) {
				spots.emplace_back(x, y);
			}
		}
	}
	return spots;
}

/**
 * Rate limit for log lines: due() is true at most once per period
 * (simulation time), suppressed calls are counted.
 */
class LogThrottle {
public:
	explicit LogThrottle(double period = 5.0) :
		period{period} {}

	bool due(double now) {
		if (now >= this->last + this->period or now < this->last) {
			this->last = now;
			return true;
		}
		this->suppressed += 1;
		return false;
	}

	/// suppressed calls since the last due(), resets the counter
	size_t take_suppressed() {
		auto s = this->suppressed;
		this->suppressed = 0;
		return s;
	}

private:
	double period;
	double last = -1e30;
	size_t suppressed = 0;
};

} // namespace openage::gamestate::ai
