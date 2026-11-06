// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gamestate/combat/rules.h"


/*
 * Combat behaviour of the XR fork, round W4 (docs/aoe2-spec 05, 06, 07):
 * projectiles, elevation, stances, target choice, garrison arrows, town bell,
 * flight of villagers and formations. Free of engine dependencies like
 * rules.h (host test combat/behavior_check.cpp); the game state side
 * (combat_state.cpp) reads the numbers from nyan and calls these functions.
 * Written from the clean-room specification, no code of the original.
 */
namespace openage::gamestate::combat {

// ---------------------------------------------------------------- damage (05 §5.2)

/**
 * Damage of one hit with a multiplier (AoE II formula):
 * D = max(1, sum over the classes with armor entry of max(0, (a - R) * m * g)),
 * with m = elevation factor x hit factor and g = terrain factor of the target.
 * The fallback effect (".Fallback") keeps its floor; factor 1 gives exactly
 * compute_damage(). Health is an integer in the engine: the sum is rounded.
 *
 * @param factor m * g (> 0).
 */
inline int64_t compute_damage(const AttackProfile &attack, const ArmorProfile &armor, double factor) {
	if (not(factor > 0.0) or not std::isfinite(factor)) {
		factor = 1.0;
	}
	double total = 0.0;
	int64_t floor = MIN_DAMAGE;
	for (const auto &effect : attack.effects) {
		if (ends_with(effect.type, FALLBACK_SUFFIX)) {
			floor = std::max(effect.min_amount, effect.amount);
			continue;
		}
		auto block = armor.block.find(effect.type);
		if (block == armor.block.end()) {
			continue;
		}
		double part = static_cast<double>(effect.amount - block->second) * factor;
		total += std::max(static_cast<double>(effect.min_amount), part);
	}
	return std::max({floor, static_cast<int64_t>(std::llround(total)), MIN_DAMAGE});
}

/// elevation factors of the specification (05 §5.2): attacker higher / lower
inline constexpr double ELEVATION_HIGH = 1.25;
inline constexpr double ELEVATION_LOW = 0.75;
/// height difference (scene up units, about one elevation level) that counts as higher
inline constexpr double ELEVATION_STEP = 0.5;

/**
 * Elevation factor of an attack (05 §5.2): x high if the attacker stands at
 * least ELEVATION_STEP above the target and no sample of the line between
 * them (half tile steps, \p line_max = highest sample) rises above the
 * attacker; x low if the target stands that much higher; else 1.
 */
inline double elevation_factor(double attacker_height,
                               double target_height,
                               double line_max,
                               double high = ELEVATION_HIGH,
                               double low = ELEVATION_LOW) {
	const double diff = attacker_height - target_height;
	if (diff >= ELEVATION_STEP and line_max <= attacker_height + 1e-9) {
		return high;
	}
	if (diff <= -ELEVATION_STEP) {
		return low;
	}
	return 1.0;
}

/**
 * Terrain factor g of the target (05 §5.2): the factor of its damage terrain
 * restriction on the tile it stands on; 0 counts as 1, no restriction = 1.
 */
inline double terrain_factor(std::optional<double> restriction_factor) {
	if (not restriction_factor or not std::isfinite(*restriction_factor) or *restriction_factor <= 0.0) {
		return 1.0;
	}
	return *restriction_factor;
}

// ---------------------------------------------------------------- projectiles (05 §5.3)

/// point with height (tiles, scene up units)
struct Point3 {
	double ne = 0.0;
	double se = 0.0;
	double up = 0.0;
};

/**
 * Flight of a projectile: straight in the ground plane, the height follows a
 * parabola over the straight line between start and aim (peak above that line).
 */
struct Flight {
	Point3 from{};
	Point3 to{};
	/// seconds from the launch to the impact (> 0)
	double duration = 0.0;
	/// highest point of the parabola above the straight line (>= 0)
	double peak = 0.0;
};

/**
 * Peak of the arc of a projectile (data field "projectile arc", converted to
 * the launch angle Projectile.arc in degrees): a parabola that leaves with
 * this angle rises d * tan(angle) / 4 over a distance d. Flat or downward
 * arcs (arrows) fly straight.
 */
inline double arc_peak(double arc_degrees, double distance) {
	if (not std::isfinite(arc_degrees) or arc_degrees <= 0.0 or distance <= 0.0) {
		return 0.0;
	}
	const double angle = std::min(arc_degrees, 75.0) * 3.14159265358979323846 / 180.0;
	return distance * std::tan(angle) / 4.0;
}

/// shortest flight (s): very close targets are hit almost at once
inline constexpr double MIN_FLIGHT = 0.05;

/**
 * Plan a flight: duration = ground distance / speed (05 §5.3, flight time =
 * way / speed), at least MIN_FLIGHT; infinite or missing speed = MIN_FLIGHT.
 */
inline Flight plan_flight(const Point3 &from, const Point3 &to, double speed, double arc_degrees) {
	Flight f;
	f.from = from;
	f.to = to;
	const double d = std::hypot(to.ne - from.ne, to.se - from.se);
	f.duration = speed > 0.0 and std::isfinite(speed) ? std::max(MIN_FLIGHT, d / speed) : MIN_FLIGHT;
	f.peak = arc_peak(arc_degrees, d);
	return f;
}

/**
 * Position of a projectile \p t seconds after the launch (clamped to the flight).
 */
inline Point3 flight_at(const Flight &f, double t) {
	const double s = f.duration > 0.0 ? std::clamp(t / f.duration, 0.0, 1.0) : 1.0;
	Point3 p;
	p.ne = f.from.ne + (f.to.ne - f.from.ne) * s;
	p.se = f.from.se + (f.to.se - f.from.se) * s;
	p.up = f.from.up + (f.to.up - f.from.up) * s + 4.0 * f.peak * s * (1.0 - s);
	return p;
}

/**
 * Hit roll of a shot (05 §5.3): z in [0, 100], hit if z <= accuracy.
 */
inline bool roll_hit(double z, double accuracy) {
	return z <= accuracy;
}

/**
 * Aim point of a missed shot (05 §5.3): the aim is moved in ne and se by
 * offsets uniform in [-s/2, s/2], s = dispersion x distance.
 *
 * @param r_ne, r_se Random numbers in [0, 1).
 */
inline Point miss_point(const Point &aim, double dispersion, double distance, double r_ne, double r_se) {
	const double s = std::max(0.0, dispersion) * std::max(0.0, distance);
	return {aim.x + (r_ne - 0.5) * s, aim.y + (r_se - 0.5) * s};
}

/**
 * Aim with "lead" (ballistics, target mode ExpectedPosition): where a target
 * moving with \p velocity (tiles/s) is when a projectile of \p speed reaches
 * it (first positive solution of |target + v t - shooter| = speed t); the
 * current position if there is none (target faster than the projectile).
 */
inline Point lead_point(const Point &shooter, const Point &target, const Point &velocity, double speed) {
	if (not(speed > 0.0) or not std::isfinite(speed)) {
		return target;
	}
	const double dx = target.x - shooter.x;
	const double dy = target.y - shooter.y;
	const double a = velocity.x * velocity.x + velocity.y * velocity.y - speed * speed;
	const double b = 2.0 * (dx * velocity.x + dy * velocity.y);
	const double c = dx * dx + dy * dy;
	double t = -1.0;
	if (std::abs(a) < 1e-12) {
		if (std::abs(b) > 1e-12) {
			t = -c / b;
		}
	}
	else {
		const double disc = b * b - 4.0 * a * c;
		if (disc >= 0.0) {
			const double sq = std::sqrt(disc);
			const double t1 = (-b - sq) / (2.0 * a);
			const double t2 = (-b + sq) / (2.0 * a);
			const double lo = std::min(t1, t2);
			const double hi = std::max(t1, t2);
			t = lo > 0.0 ? lo : hi;
		}
	}
	if (not(t > 0.0) or not std::isfinite(t)) {
		return target;
	}
	return {target.x + velocity.x * t, target.y + velocity.y * t};
}

/**
 * Height of the aim point (05 §5.3): ground of the target + 1/3 of its height.
 */
inline double aim_height(double ground, double target_height) {
	return ground + std::max(0.0, target_height) / 3.0;
}

/**
 * Whether a missed projectile touches a footprint (05 §5.3, "first object it
 * touches"): only the last part of the flight is near the ground; the
 * segment from \p a to \p b (ground plane) is tested against the footprint.
 *
 * @return Fraction along a -> b of the first contact, nothing if none.
 */
inline std::optional<double> segment_touches(const Point &a, const Point &b, const Footprint &f) {
	constexpr int samples = 16;
	for (int i = 0; i <= samples; ++i) {
		const double s = static_cast<double>(i) / samples;
		const Point p{a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s};
		const Point q = nearest_point(f, p);
		if (distance(p, q) <= 1e-9) {
			return s;
		}
	}
	return std::nullopt;
}

/// share of the flight in which a missed projectile can touch objects (low enough)
inline constexpr double MISS_TOUCH_SHARE = 0.25;

// ---------------------------------------------------------------- stances (06 §6.2)

enum class stance_t : uint8_t {
	AGGRESSIVE = 0,
	DEFENSIVE = 1,
	STAND_GROUND = 2,
	NO_ATTACK = 3,
};

inline constexpr size_t STANCE_COUNT = 4;

inline const char *to_string(stance_t s) {
	switch (s) {
	case stance_t::AGGRESSIVE:
		return "aggressive";
	case stance_t::DEFENSIVE:
		return "defensive";
	case stance_t::STAND_GROUND:
		return "stand_ground";
	case stance_t::NO_ATTACK:
		return "no_attack";
	default:
		return "?";
	}
}

inline std::optional<stance_t> stance_from_string(const std::string &s) {
	for (size_t i = 0; i < STANCE_COUNT; ++i) {
		auto stance = static_cast<stance_t>(i);
		if (s == to_string(stance)) {
			return stance;
		}
	}
	return std::nullopt;
}

/// defensive: targets up to R + 5 from the post, back once more than 5 away
inline constexpr double DEFENSIVE_REACH = 5.0;
inline constexpr double DEFENSIVE_LEASH = 5.0;
/// stand ground: R + 0.5 (gaia animals R + 2), back once more than 0.5 away
inline constexpr double STAND_GROUND_REACH = 0.5;
inline constexpr double STAND_GROUND_ANIMAL_REACH = 2.0;
inline constexpr double STAND_GROUND_LEASH = 0.5;

/**
 * May a unit in \p stance attack a target automatically (06 §6.2)?
 *
 * @param from_post Edge distance of the target from the post of the unit
 *                  (patrol/guard: from the route or the guarded object).
 * @param range Maximum weapon range R (melee: MELEE_RANGE).
 * @param animal Target is a gaia animal (stand ground: R + 2).
 */
inline bool stance_allows(stance_t stance, double from_post, double range, bool animal = false) {
	switch (stance) {
	case stance_t::AGGRESSIVE:
		return true;
	case stance_t::DEFENSIVE:
		return from_post <= range + DEFENSIVE_REACH + RANGE_EPSILON;
	case stance_t::STAND_GROUND:
		return from_post <= range + (animal ? STAND_GROUND_ANIMAL_REACH : STAND_GROUND_REACH) + RANGE_EPSILON;
	case stance_t::NO_ATTACK:
	default:
		return false;
	}
}

/**
 * Does a unit in \p stance give up the fight and walk back to its post
 * (06 §6.2: defensive > 5 tiles away, stand ground > 0.5)?
 */
inline bool stance_returns(stance_t stance, double from_post) {
	switch (stance) {
	case stance_t::DEFENSIVE:
		return from_post > DEFENSIVE_LEASH;
	case stance_t::STAND_GROUND:
		return from_post > STAND_GROUND_LEASH;
	default:
		return false;
	}
}

/**
 * May a unit in \p stance chase a target (walk towards it)? Stand ground only
 * turns and shoots/hits what is in range; the others walk.
 */
inline bool stance_moves(stance_t stance) {
	return stance == stance_t::AGGRESSIVE or stance == stance_t::DEFENSIVE;
}

/**
 * Radius of the surroundings list of a unit (06 §6.2): max(sight + 1, range).
 */
inline double scan_radius(double sight, double range) {
	return std::max(sight + 1.0, range);
}

// ---------------------------------------------------------------- target choice (06 §6.2)

/// target categories of the threat memory
enum class category_t : uint8_t {
	BUILDING = 0,
	INFANTRY,
	ARCHER,
	CAVALRY,
	SIEGE,
	SHIP,
	CIVILIAN,
	OTHER,
};

inline constexpr size_t CATEGORY_COUNT = 8;

/**
 * Category of an object from its unit class (data field, docs/aoe2-spec 05 §5.1).
 */
inline category_t category_of(int unit_class, bool building, bool villager) {
	if (building) {
		return category_t::BUILDING;
	}
	if (villager) {
		return category_t::CIVILIAN;
	}
	switch (unit_class) {
	case 0:   // archer
	case 36:  // cavalry archer
	case 44:  // hand cannoneer
	case 23:  // conquistador
		return category_t::ARCHER;
	case 6:   // infantry
	case 35:  // pikeman
		return category_t::INFANTRY;
	case 12:  // cavalry
	case 47:  // scout cavalry
	case 37:  // cavalry raider
		return category_t::CAVALRY;
	case 13:  // siege weapon
	case 51:  // packed trebuchet
	case 54:  // unpacked trebuchet
	case 55:  // scorpion
		return category_t::SIEGE;
	case 2:   // trade boat
	case 21:  // fishing boat
	case 22:  // warship
	case 20:  // transport boat
	case 53:
		return category_t::SHIP;
	case 4:   // civilian
	case 18:  // monk
	case 19:  // trade cart
		return category_t::CIVILIAN;
	default:
		return category_t::OTHER;
	}
}

/// threat memory: attack reports of the last THREAT_SECONDS count with weight (60 - age) / 60
inline constexpr double THREAT_SECONDS = 60.0;

class ThreatMemory {
public:
	/// an object of category \p k attacked at \p time
	void add(double time, category_t k) {
		this->reports.push_back({time, k});
	}

	/// forget reports older than THREAT_SECONDS
	void prune(double now) {
		std::erase_if(this->reports, [now](const auto &r) { return now - r.first >= THREAT_SECONDS; });
	}

	/// summed weight of category k
	double weight(double now, category_t k) const {
		double w = 0.0;
		for (const auto &[t, cat] : this->reports) {
			const double age = now - t;
			if (cat == k and age >= 0.0 and age < THREAT_SECONDS) {
				w += (THREAT_SECONDS - age) / THREAT_SECONDS;
			}
		}
		return w;
	}

	/// weights of all categories, normalised to 0..1 by the largest one
	std::array<double, CATEGORY_COUNT> normalized(double now) const {
		std::array<double, CATEGORY_COUNT> w{};
		double top = 0.0;
		for (size_t k = 0; k < CATEGORY_COUNT; ++k) {
			w[k] = this->weight(now, static_cast<category_t>(k));
			top = std::max(top, w[k]);
		}
		if (top > 0.0) {
			for (auto &v : w) {
				v /= top;
			}
		}
		return w;
	}

	size_t size() const {
		return this->reports.size();
	}

	const std::vector<std::pair<double, category_t>> &get_reports() const {
		return this->reports;
	}

private:
	std::vector<std::pair<double, category_t>> reports{};
};

/// one candidate of the automatic target choice
struct Candidate {
	uint64_t id = 0;
	/// edge distance (tiles)
	double distance = 0.0;
	/// time to kill: health / damage per hit / reload time (06 §6.2)
	double kill_time = 0.0;
	category_t category = category_t::OTHER;
	/// the current target of the unit (+5)
	bool current = false;
	/// can move (normalised separately from buildings and other fixed objects)
	bool movable = true;
};

/// points of the target choice
inline constexpr double SCORE_DISTANCE = 75.0;
inline constexpr double SCORE_KILL = 10.0;
inline constexpr double SCORE_THREAT = 10.0;
inline constexpr double SCORE_CURRENT = 5.0;
/// tried targets per choice, lock of a failed target (s)
inline constexpr size_t TARGET_TRIES = 3;
inline constexpr double TARGET_LOCK = 10.0;

/**
 * Score of every candidate (06 §6.2): S = S_d + S_t + S_h + B with
 * S_d = 75 (1 - (d - dmin)/(dmax - dmin)), S_t = 10 (1 - (T - Tmin)/(Tmax - Tmin)),
 * S_h = 10 w_k, B = 5 for the current target; the ranges are taken separately
 * for movable and fixed candidates (a single value scores full points).
 */
inline std::vector<double> score_targets(const std::vector<Candidate> &candidates,
                                         const std::array<double, CATEGORY_COUNT> &threat) {
	std::vector<double> scores(candidates.size(), 0.0);
	for (int group = 0; group < 2; ++group) {
		const bool movable = group == 0;
		double dmin = std::numeric_limits<double>::infinity(), dmax = -dmin;
		double tmin = dmin, tmax = -dmin;
		for (const auto &c : candidates) {
			if (c.movable != movable) {
				continue;
			}
			dmin = std::min(dmin, c.distance);
			dmax = std::max(dmax, c.distance);
			tmin = std::min(tmin, c.kill_time);
			tmax = std::max(tmax, c.kill_time);
		}
		for (size_t i = 0; i < candidates.size(); ++i) {
			const auto &c = candidates[i];
			if (c.movable != movable) {
				continue;
			}
			const double sd = dmax - dmin > 1e-9 ? 1.0 - (c.distance - dmin) / (dmax - dmin) : 1.0;
			const double st = tmax - tmin > 1e-9 ? 1.0 - (c.kill_time - tmin) / (tmax - tmin) : 1.0;
			const double sh = threat[static_cast<size_t>(c.category)];
			scores[i] = SCORE_DISTANCE * sd + SCORE_KILL * st + SCORE_THREAT * sh + (c.current ? SCORE_CURRENT : 0.0);
		}
	}
	return scores;
}

/**
 * The best candidates in order (at most \p count), ties by distance and id
 * (deterministic).
 */
inline std::vector<size_t> best_targets(const std::vector<Candidate> &candidates,
                                        const std::array<double, CATEGORY_COUNT> &threat,
                                        size_t count = TARGET_TRIES) {
	const auto scores = score_targets(candidates, threat);
	std::vector<size_t> order(candidates.size());
	for (size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		if (std::abs(scores[a] - scores[b]) > 1e-9) {
			return scores[a] > scores[b];
		}
		if (candidates[a].distance != candidates[b].distance) {
			return candidates[a].distance < candidates[b].distance;
		}
		return candidates[a].id < candidates[b].id;
	});
	if (order.size() > count) {
		order.resize(count);
	}
	return order;
}

/**
 * Time to kill a target (06 §6.2): health / damage per hit x reload time
 * (the specification writes it as HP / damage / reload; both orders rank
 * targets of one attacker the same way, this one is in seconds).
 */
inline double kill_time(double health, double damage, double reload) {
	const double hits = health / std::max(1.0, damage);
	return hits * std::max(0.1, reload);
}

// ---------------------------------------------------------------- garrison (05 §5.4, 06 §6.5)

/// garrison type mask (data field "garrison type", 06 §6.5, bit meaning observed by values)
inline constexpr int GARRISON_VILLAGER = 1;
inline constexpr int GARRISON_INFANTRY = 2;
inline constexpr int GARRISON_CAVALRY = 4;
inline constexpr int GARRISON_MONK = 8;

/// kind of a unit for the garrison mask
enum class garrison_kind_t : uint8_t {
	VILLAGER,
	INFANTRY,
	CAVALRY,
	MONK,
	NONE,
};

/**
 * Garrison kind of a unit from its unit class: villagers (4), foot soldiers and
 * foot archers (infantry 6, archers 0, pikemen 35, hand cannoneers 44),
 * mounted units (12, 23, 36, 37, 47) and monks (18); siege, ships, … cannot
 * enter buildings.
 */
inline garrison_kind_t garrison_kind(int unit_class, bool villager) {
	if (villager or unit_class == 4) {
		return garrison_kind_t::VILLAGER;
	}
	switch (unit_class) {
	case 0:
	case 6:
	case 35:
	case 44:
		return garrison_kind_t::INFANTRY;
	case 12:
	case 23:
	case 36:
	case 37:
	case 47:
		return garrison_kind_t::CAVALRY;
	case 18:
		return garrison_kind_t::MONK;
	default:
		return garrison_kind_t::NONE;
	}
}

/**
 * Does a ram take a unit of this class? Foot soldiers only (infantry 6, pikemen 35), unlike
 * garrison_kind_t::INFANTRY, which counts foot archers for the buildings (spec 06).
 */
inline bool ram_accepts(int unit_class) {
	return unit_class == 6 or unit_class == 35;
}

/**
 * Does a building with garrison type \p mask accept a unit of \p kind?
 */
inline bool garrison_accepts(int mask, garrison_kind_t kind) {
	switch (kind) {
	case garrison_kind_t::VILLAGER:
		return (mask & GARRISON_VILLAGER) != 0;
	case garrison_kind_t::INFANTRY:
		return (mask & GARRISON_INFANTRY) != 0;
	case garrison_kind_t::CAVALRY:
		return (mask & GARRISON_CAVALRY) != 0;
	case garrison_kind_t::MONK:
		return (mask & GARRISON_MONK) != 0;
	default:
		return false;
	}
}

/// fixed arrow DPS of a garrisoned villager and of infantry with crenellations (05 §5.4)
inline constexpr double GARRISON_VILLAGER_DPS = 2.5;

/**
 * Arrow contribution of one garrisoned unit (05 §5.4): only archers (0), hand
 * cannoneers (44), villagers (4), cavalry archers (36), conquistadors (23)
 * and infantry (6) count; Delta n = DPS of the unit / DPS of the building's
 * arrow (villagers fixed 2.5, infantry 2.5 only with crenellations).
 *
 * @param unit_dps Attack / reload of the unit (ranged: of its projectile).
 * @param building_dps Attack / reload of the building's arrow.
 */
inline double arrow_contribution(int unit_class, bool villager, double unit_dps, double building_dps, bool crenellations) {
	if (not(building_dps > 0.0)) {
		return 0.0;
	}
	double dps = 0.0;
	if (villager or unit_class == 4) {
		dps = GARRISON_VILLAGER_DPS;
	}
	else if (unit_class == 6) {
		dps = crenellations ? GARRISON_VILLAGER_DPS : 0.0;
	}
	else if (unit_class == 0 or unit_class == 44 or unit_class == 36 or unit_class == 23) {
		dps = std::max(0.0, unit_dps);
	}
	return dps / building_dps;
}

/**
 * Arrows of one volley (05 §5.4): N = min(max of the data, whole part of the
 * running value); the running value is the base (min count) plus the
 * contributions, 0 = the building does not shoot.
 */
inline int arrow_count(double running, int max_count) {
	if (not(running > 0.0) or not std::isfinite(running)) {
		return 0;
	}
	return std::clamp(static_cast<int>(std::floor(running + 1e-9)), 0, std::max(0, max_count));
}

/**
 * Point of an extra arrow on the spawning area (05 §5.4): uniform on a
 * rectangle width x length around the aim, the long side across the shot.
 *
 * @param dir_x, dir_y Direction of the shot (normalised inside).
 * @param r1, r2 Random numbers in [0, 1).
 */
inline Point area_point(const Point &aim, double dir_x, double dir_y, double width, double length, double r1, double r2) {
	double len = std::hypot(dir_x, dir_y);
	if (len < 1e-9) {
		dir_x = 1.0;
		dir_y = 0.0;
		len = 1.0;
	}
	const double ux = dir_x / len, uy = dir_y / len;
	const double across = (r1 - 0.5) * std::max(0.0, width);
	const double along = (r2 - 0.5) * std::max(0.0, length);
	return {aim.x + ux * along - uy * across, aim.y + uy * along + ux * across};
}

/**
 * Healing in a garrison (05 §5.4: garrison heal rate, HP per second): the
 * fractional part is carried over between ticks.
 *
 * @param carry In/out: health fraction not applied yet.
 * @return Whole health points to add now (never above \p missing).
 */
inline int64_t garrison_heal(double rate, double dt, double &carry, int64_t missing) {
	if (missing <= 0 or not(rate > 0.0) or not(dt > 0.0)) {
		carry = 0.0;
		return 0;
	}
	carry += rate * dt;
	auto whole = static_cast<int64_t>(std::floor(carry + 1e-9));
	whole = std::min(whole, missing);
	carry -= static_cast<double>(whole);
	if (whole >= missing) {
		carry = 0.0;
	}
	return whole;
}

// ---------------------------------------------------------------- town bell (06 §6.4)

/// half side of the bell area (square around the town center, tiles)
inline constexpr double BELL_RANGE = 25.0;
/// ring step of the distribution (squared tiles) and the last ring (25^2)
inline constexpr double BELL_RING_STEP = 5.0;
inline constexpr double BELL_RING_MAX = 625.0;

inline bool in_bell_area(const Point &center, const Point &p) {
	return std::abs(p.x - center.x) <= BELL_RANGE and std::abs(p.y - center.y) <= BELL_RANGE;
}

struct BellVillager {
	uint64_t id = 0;
	Point pos{};
};

struct BellShelter {
	uint64_t id = 0;
	Point pos{};
	/// free places (capacity - occupants)
	int free = 0;
};

/**
 * Distribute the villagers on the shelters (06 §6.4): in growing rings of the
 * squared distance (5, 10, … 625) every villager without a place takes the
 * nearest shelter with a free place once its squared distance to that shelter
 * falls into the ring; the nearest go first. Villagers without a place stay.
 *
 * @return (villager id, shelter id) pairs.
 */
inline std::vector<std::pair<uint64_t, uint64_t>> assign_bell(const std::vector<BellVillager> &villagers,
                                                              std::vector<BellShelter> shelters) {
	std::vector<std::pair<uint64_t, uint64_t>> result;
	std::vector<bool> done(villagers.size(), false);
	for (double ring = BELL_RING_STEP; ring <= BELL_RING_MAX + 1e-9; ring += BELL_RING_STEP) {
		// candidates of this ring, nearest first
		std::vector<std::tuple<double, size_t, size_t>> pending;
		for (size_t v = 0; v < villagers.size(); ++v) {
			if (done[v]) {
				continue;
			}
			std::optional<size_t> best;
			double best_d = std::numeric_limits<double>::infinity();
			for (size_t s = 0; s < shelters.size(); ++s) {
				if (shelters[s].free <= 0) {
					continue;
				}
				const double dx = villagers[v].pos.x - shelters[s].pos.x;
				const double dy = villagers[v].pos.y - shelters[s].pos.y;
				const double d2 = dx * dx + dy * dy;
				if (d2 < best_d or (d2 == best_d and best and shelters[s].id < shelters[*best].id)) {
					best_d = d2;
					best = s;
				}
			}
			if (best and best_d <= ring) {
				pending.emplace_back(best_d, v, *best);
			}
		}
		std::sort(pending.begin(), pending.end(), [&](const auto &a, const auto &b) {
			if (std::get<0>(a) != std::get<0>(b)) {
				return std::get<0>(a) < std::get<0>(b);
			}
			return villagers[std::get<1>(a)].id < villagers[std::get<1>(b)].id;
		});
		for (const auto &[d2, v, s] : pending) {
			if (shelters[s].free <= 0) {
				continue;  // taken by a nearer villager of this ring: next ring
			}
			shelters[s].free -= 1;
			done[v] = true;
			result.emplace_back(villagers[v].id, shelters[s].id);
		}
	}
	return result;
}

// ---------------------------------------------------------------- flight of villagers (06 §6.4)

/**
 * Where a villager flees to (06 §6.4): away from the sum of the directions
 * to all current attackers (no attackers: \p random_angle), 2 x its speed
 * tiles; in the defensive stance at most DEFENSIVE_LEASH from its post.
 */
inline Point flee_point(const Point &self,
                        const std::vector<Point> &attackers,
                        double speed,
                        double random_angle,
                        const std::optional<Point> &post = std::nullopt) {
	double dx = 0.0, dy = 0.0;
	for (const auto &a : attackers) {
		double ax = a.x - self.x, ay = a.y - self.y;
		const double len = std::hypot(ax, ay);
		if (len > 1e-9) {
			dx += ax / len;
			dy += ay / len;
		}
	}
	double len = std::hypot(dx, dy);
	if (len < 1e-9) {
		dx = -std::cos(random_angle);
		dy = -std::sin(random_angle);
		len = 1.0;
	}
	const double way = 2.0 * std::max(0.0, speed);
	Point p{self.x - dx / len * way, self.y - dy / len * way};
	if (post) {
		const double d = distance(p, *post);
		if (d > DEFENSIVE_LEASH) {
			p = {post->x + (p.x - post->x) / d * DEFENSIVE_LEASH, post->y + (p.y - post->y) / d * DEFENSIVE_LEASH};
		}
	}
	return p;
}

/// a villager is calm again (resumes its work) this long after the last attack report (s)
inline constexpr double FLEE_CALM = 3.0;

// ---------------------------------------------------------------- formations (07 §7.5)

enum class formation_t : uint8_t {
	NONE = 0,
	LINE,
	BOX,
	STAGGERED,
	FLANK,
};

inline const char *to_string(formation_t f) {
	switch (f) {
	case formation_t::LINE:
		return "line";
	case formation_t::BOX:
		return "box";
	case formation_t::STAGGERED:
		return "staggered";
	case formation_t::FLANK:
		return "flank";
	case formation_t::NONE:
	default:
		return "none";
	}
}

inline std::optional<formation_t> formation_from_string(const std::string &s) {
	for (auto f : {formation_t::NONE, formation_t::LINE, formation_t::BOX, formation_t::STAGGERED, formation_t::FLANK}) {
		if (s == to_string(f)) {
			return f;
		}
	}
	return std::nullopt;
}

/**
 * Formation parameters. The data names only the order of the subformations
 * (Subformation.ordering_priority, cavalry 5 … support 1, the front row has
 * the highest); spacing and proportions follow the public descriptions of
 * the original (L54 open): rows about twice as wide as deep, one tile apart.
 */
struct FormationParams {
	/// distance of neighbours in a row (tiles)
	double spacing = 1.0;
	/// distance of the rows (tiles)
	double row_gap = 1.0;
	/// staggered: gap between the units of a row
	double staggered_spacing = 2.0;
	/// flank: gap between the two halves
	double flank_gap = 4.0;
	/// width : depth of a line block
	double aspect = 2.0;
};

/// one member of a formation
struct FormationMember {
	uint64_t id = 0;
	/// Subformation.ordering_priority (higher = further in front)
	int priority = 0;
	/// current position (assignment to the nearest slot of its row)
	Point pos{};
};

/**
 * Slots of a formation around \p center facing \p dir (ground plane): members
 * sorted by priority into rows (front = highest priority, nearest to the
 * destination), each row centred. LINE = wide rows, STAGGERED = like line with
 * double gaps and every second row shifted, BOX = ring around the centre
 * (melee/ranged outside, support inside), FLANK = line split into two halves
 * with a gap. Deterministic.
 *
 * @return (member id, slot) pairs in the order of \p members after sorting.
 */
inline std::vector<std::pair<uint64_t, Point>> formation_slots(formation_t formation,
                                                               std::vector<FormationMember> members,
                                                               const Point &center,
                                                               const Point &dir,
                                                               const FormationParams &params = {}) {
	std::vector<std::pair<uint64_t, Point>> result;
	if (members.empty()) {
		return result;
	}
	double fx = dir.x, fy = dir.y;
	double flen = std::hypot(fx, fy);
	if (flen < 1e-9) {
		fx = 1.0;
		fy = 0.0;
		flen = 1.0;
	}
	fx /= flen;
	fy /= flen;
	// right of the facing direction
	const double rx = -fy, ry = fx;
	std::stable_sort(members.begin(), members.end(), [](const auto &a, const auto &b) {
		if (a.priority != b.priority) {
			return a.priority > b.priority;
		}
		return a.id < b.id;
	});
	const size_t n = members.size();
	auto place = [&](double across, double along) {
		return Point{center.x + rx * across + fx * along, center.y + ry * across + fy * along};
	};

	if (formation == formation_t::BOX and n >= 4) {
		// ring of the outer members (higher priority) around the others
		const size_t inner = n >= 9 ? n / 3 : 0;
		const size_t outer = n - inner;
		const double side = std::max(2.0, std::ceil(static_cast<double>(outer) / 4.0) * params.spacing);
		for (size_t i = 0; i < outer; ++i) {
			const double t = static_cast<double>(i) / static_cast<double>(outer) * 4.0;
			const int edge = static_cast<int>(std::floor(t));
			const double f = t - edge;
			double a = 0.0, b = 0.0;
			switch (edge) {
			case 0:
				a = -side / 2 + f * side;
				b = side / 2;
				break;
			case 1:
				a = side / 2;
				b = side / 2 - f * side;
				break;
			case 2:
				a = side / 2 - f * side;
				b = -side / 2;
				break;
			default:
				a = -side / 2;
				b = -side / 2 + f * side;
				break;
			}
			result.emplace_back(members[i].id, place(a, b));
		}
		const auto cols = static_cast<size_t>(std::max(1.0, std::ceil(std::sqrt(static_cast<double>(inner)))));
		for (size_t j = 0; j < inner; ++j) {
			const size_t r = j / cols, c = j % cols;
			const double across = (static_cast<double>(c) - (static_cast<double>(cols) - 1.0) / 2.0) * params.spacing * 0.8;
			const double along = -(static_cast<double>(r) - (static_cast<double>((inner + cols - 1) / cols) - 1.0) / 2.0)
			                     * params.spacing * 0.8;
			result.emplace_back(members[outer + j].id, place(across, along));
		}
		return result;
	}

	const double gap = formation == formation_t::STAGGERED ? params.staggered_spacing : params.spacing;
	// width of a row: about aspect x depth, at least 2
	auto per_row = static_cast<size_t>(std::max(2.0, std::ceil(std::sqrt(static_cast<double>(n) * params.aspect))));
	per_row = std::min(per_row, n);
	const size_t rows = (n + per_row - 1) / per_row;
	for (size_t i = 0; i < n; ++i) {
		const size_t r = i / per_row;
		const size_t in_row = std::min(per_row, n - r * per_row);
		const size_t c = i % per_row;
		double across = (static_cast<double>(c) - (static_cast<double>(in_row) - 1.0) / 2.0) * gap;
		if (formation == formation_t::STAGGERED and r % 2 == 1) {
			across += gap / 2.0;
		}
		if (formation == formation_t::FLANK) {
			// two halves: the left half moves left, the right half right
			const double half = (static_cast<double>(in_row) - 1.0) / 2.0;
			if (static_cast<double>(c) <= half - 1e-9) {
				across -= params.flank_gap / 2.0;
			}
			else if (static_cast<double>(c) >= half + 1e-9) {
				across += params.flank_gap / 2.0;
			}
		}
		const double along = ((static_cast<double>(rows) - 1.0) / 2.0 - static_cast<double>(r)) * params.row_gap;
		result.emplace_back(members[i].id, place(across, along));
	}
	return result;
}

/**
 * Speed of a group (07 §7.5): the slowest member.
 */
inline double group_speed(const std::vector<double> &speeds) {
	double v = std::numeric_limits<double>::infinity();
	for (double s : speeds) {
		if (s > 0.0 and std::isfinite(s)) {
			v = std::min(v, s);
		}
	}
	return std::isfinite(v) ? v : 0.0;
}

// ---------------------------------------------------------------- orders (06 §6.3)

enum class order_t : uint8_t {
	NONE = 0,
	ATTACK,
	PATROL,
	GUARD,
	FOLLOW,
	ATTACK_MOVE,
};

inline const char *to_string(order_t o) {
	switch (o) {
	case order_t::ATTACK:
		return "attack";
	case order_t::PATROL:
		return "patrol";
	case order_t::GUARD:
		return "guard";
	case order_t::FOLLOW:
		return "follow";
	case order_t::ATTACK_MOVE:
		return "attack_move";
	case order_t::NONE:
	default:
		return "none";
	}
}

inline std::optional<order_t> order_from_string(const std::string &s) {
	for (auto o : {order_t::NONE, order_t::ATTACK, order_t::PATROL, order_t::GUARD, order_t::FOLLOW, order_t::ATTACK_MOVE}) {
		if (s == to_string(o)) {
			return o;
		}
	}
	return std::nullopt;
}

/// guard: stay within this distance of the guarded object (tiles)
inline constexpr double GUARD_DISTANCE = 2.0;
/// follow: keep this distance (large: scouting, 06 §6.3)
inline constexpr double FOLLOW_DISTANCE = 4.0;
/// a patrol leg ends this close to its point
inline constexpr double PATROL_ARRIVE = 0.75;

/**
 * Distance of \p p from the segment \p a - \p b in the ground plane (patrol route).
 */
inline double route_distance(const Point &p, const Point &a, const Point &b) {
	return point_segment_distance(p, a, b);
}

} // namespace openage::gamestate::combat
