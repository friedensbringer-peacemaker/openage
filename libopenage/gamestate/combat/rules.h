// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>


/*
 * Combat rules of the XR fork, free of engine dependencies (host-testable,
 * see combat/rules_check.cpp). The game state side (combat_state.h) reads the
 * values from nyan and calls these functions.
 */
namespace openage::gamestate::combat {

/**
 * One discrete flat attribute change of an attack (AoE II: one attack class).
 */
struct Effect {
	/// attribute change type (fqon), e.g. "...attribute_change_type.types.Melee"
	std::string type;
	/// change value (health points)
	int64_t amount = 0;
	/// lower bound of the change after armor (FlatAttributeChange.min_change_value)
	int64_t min_amount = 0;
};

/**
 * Attack values of an attacker (effects of its ApplyDiscreteEffect batches,
 * for ranged units those of the projectile).
 */
struct AttackProfile {
	std::vector<Effect> effects;
};

/**
 * Armor of a target: blocked amount per attribute change type (Resistance).
 */
struct ArmorProfile {
	std::unordered_map<std::string, int64_t> block;
};

/// suffix of the fallback attribute change type (AoE II: minimum damage 1)
inline constexpr const char *FALLBACK_SUFFIX = ".Fallback";

/// damage of a hit that matches no armor class (and the floor without fallback effect)
inline constexpr int64_t MIN_DAMAGE = 1;

/// melee: hitboxes closer than this (tiles) are in contact
inline constexpr double MELEE_RANGE = 0.5;

/// line of sight if the game entity has none (tiles)
inline constexpr double DEFAULT_LINE_OF_SIGHT = 6.0;

/// tolerance for range checks (tiles)
inline constexpr double RANGE_EPSILON = 0.05;

inline bool ends_with(const std::string &text, const std::string &suffix) {
	return text.size() >= suffix.size()
	       and text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/**
 * Damage of one hit (AoE II formula).
 *
 * Sum over the attack classes the target has armor for of
 * max(min_amount, attack - armor); classes without armor entry do not apply.
 * The result is at least the fallback effect's minimum (MIN_DAMAGE if the
 * attack has no fallback effect), so every hit does damage:
 * max(1, attack - armor) for a single class.
 *
 * @param attack Attack values.
 * @param armor Armor of the target.
 *
 * @return Damage in health points (>= 1).
 */
inline int64_t compute_damage(const AttackProfile &attack, const ArmorProfile &armor) {
	int64_t total = 0;
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
		total += std::max(effect.min_amount, effect.amount - block->second);
	}
	return std::max({floor, total, MIN_DAMAGE});
}

/**
 * Sum of the attack values (for logs and the HUD).
 */
inline int64_t attack_sum(const AttackProfile &attack) {
	int64_t sum = 0;
	for (const auto &effect : attack.effects) {
		if (not ends_with(effect.type, FALLBACK_SUFFIX)) {
			sum += effect.amount;
		}
	}
	return sum;
}

/**
 * Distance between the edges of two hitboxes (circles in the ground plane).
 *
 * @param center_distance Distance of the centers (tiles).
 * @param radius_a Hitbox radius of the first entity.
 * @param radius_b Hitbox radius of the second entity.
 */
inline double edge_distance(double center_distance, double radius_a, double radius_b) {
	return std::max(0.0, center_distance - radius_a - radius_b);
}

/**
 * Whether a target at edge distance \p edge can be attacked.
 *
 * @param edge Edge distance (see edge_distance()).
 * @param min_range Minimum range of a ranged attack (0 for melee).
 * @param max_range Maximum range; <= 0 means melee (MELEE_RANGE).
 */
inline bool in_attack_range(double edge, double min_range, double max_range) {
	double max = max_range > 0.0 ? max_range : MELEE_RANGE;
	return edge + RANGE_EPSILON >= min_range and edge <= max + RANGE_EPSILON;
}

/**
 * Point in the ground plane.
 */
struct Point {
	double x = 0.0;
	double y = 0.0;
};

inline double distance(const Point &a, const Point &b) {
	return std::hypot(a.x - b.x, a.y - b.y);
}

/**
 * Shape of an entity in the ground plane: units are circles, buildings
 * axis-aligned squares (their footprint of whole tiles).
 */
struct Footprint {
	Point center{};
	double radius = 0.25;
	bool square = false;
};

/**
 * Nearest point of a footprint to \p p (p itself if inside).
 */
inline Point nearest_point(const Footprint &f, const Point &p) {
	if (f.square) {
		return {std::clamp(p.x, f.center.x - f.radius, f.center.x + f.radius),
		        std::clamp(p.y, f.center.y - f.radius, f.center.y + f.radius)};
	}
	double d = distance(p, f.center);
	if (d <= f.radius or d < 1e-12) {
		return p;
	}
	return {f.center.x + (p.x - f.center.x) / d * f.radius,
	        f.center.y + (p.y - f.center.y) / d * f.radius};
}

/**
 * Edge distance between two footprints (0 if they overlap). Square to
 * square uses the gap between the boxes; otherwise the circle radius is
 * subtracted from the distance to the nearest point of the other footprint.
 */
inline double edge_distance(const Footprint &a, const Footprint &b) {
	if (a.square and b.square) {
		double gx = std::max(0.0, std::abs(a.center.x - b.center.x) - a.radius - b.radius);
		double gy = std::max(0.0, std::abs(a.center.y - b.center.y) - a.radius - b.radius);
		return std::hypot(gx, gy);
	}
	if (a.square) {
		return edge_distance(b, a);
	}
	// a is a circle
	auto q = nearest_point(b, a.center);
	return std::max(0.0, distance(a.center, q) - a.radius);
}

/**
 * Where an attacker (circle with radius \p attacker_radius) walks to have the
 * edge distance \p gap to the target footprint: from the nearest point of the
 * target outwards towards the attacker. If the attacker stands inside the
 * target, the point lies in direction \p fallback_angle (radians).
 */
inline Point approach_point(const Point &attacker,
                            double attacker_radius,
                            const Footprint &target,
                            double gap,
                            double fallback_angle = 0.0) {
	auto q = nearest_point(target, attacker);
	double dx = attacker.x - q.x;
	double dy = attacker.y - q.y;
	double len = std::hypot(dx, dy);
	if (len < 1e-9) {
		// inside or on the edge: leave in the fallback direction
		dx = std::cos(fallback_angle);
		dy = std::sin(fallback_angle);
		double reach = target.square ? target.radius * std::sqrt(2.0) : target.radius;
		q = {target.center.x + dx * reach, target.center.y + dy * reach};
		len = 1.0;
	}
	double want = attacker_radius + gap;
	if (len <= want) {
		return attacker;
	}
	return {q.x + dx / len * want, q.y + dy / len * want};
}

/**
 * Distance of the point \p p to the segment \p a - \p b (screen space picking).
 */
inline double point_segment_distance(const Point &p, const Point &a, const Point &b) {
	double vx = b.x - a.x;
	double vy = b.y - a.y;
	double len2 = vx * vx + vy * vy;
	double t = 0.0;
	if (len2 > 1e-18) {
		t = std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / len2, 0.0, 1.0);
	}
	return distance(p, {a.x + t * vx, a.y + t * vy});
}

/**
 * State of a match for one player.
 */
enum class match_state_t {
	/// fewer than two players ever had units or buildings, or more than one still has
	RUNNING,
	/// the player is the only one left
	VICTORY,
	/// the player lost all units and buildings
	DEFEAT,
	/// nobody is left (last units died at the same time)
	DRAW,
};

inline const char *to_string(match_state_t state) {
	switch (state) {
	case match_state_t::RUNNING:
		return "running";
	case match_state_t::VICTORY:
		return "victory";
	case match_state_t::DEFEAT:
		return "defeat";
	case match_state_t::DRAW:
		return "draw";
	default:
		return "?";
	}
}

/**
 * Result of the victory condition "all units and buildings of the other
 * players are destroyed" (Gaia/neutral players are not counted).
 */
struct MatchResult {
	/// at least two participants and at most one of them has units or buildings left
	bool over = false;
	/// the remaining player (none on a draw or while running)
	std::optional<uint64_t> winner{};
	/// participants without units and buildings
	std::vector<uint64_t> defeated{};

	/**
	 * State of the match from the view of \p player.
	 */
	match_state_t state_for(uint64_t player) const {
		if (std::find(this->defeated.begin(), this->defeated.end(), player) != this->defeated.end()) {
			return this->over and not this->winner ? match_state_t::DRAW : match_state_t::DEFEAT;
		}
		if (this->over and this->winner and *this->winner == player) {
			return match_state_t::VICTORY;
		}
		return match_state_t::RUNNING;
	}
};

/**
 * Evaluate the victory condition.
 *
 * @param alive Remaining units + buildings per participant (players that ever
 *              had units or buildings; neutral players excluded).
 */
inline MatchResult evaluate_match(const std::map<uint64_t, size_t> &alive) {
	MatchResult result;
	size_t left = 0;
	std::optional<uint64_t> last{};
	for (const auto &[player, count] : alive) {
		if (count == 0) {
			result.defeated.push_back(player);
		}
		else {
			left += 1;
			last = player;
		}
	}
	if (alive.size() >= 2 and left <= 1) {
		result.over = true;
		result.winner = last;
	}
	return result;
}

} // namespace openage::gamestate::combat
