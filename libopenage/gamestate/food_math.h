// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gamestate/econ_math.h"


namespace openage::gamestate::food {

/**
 * Rules of the food economy (XR fork W3, docs/aoe2-spec 02 §2.4–2.6, 04 §4.2, 13):
 * herdables, hunting, carcass decay, farms with reseeding, fishing and the
 * placement of water buildings. Written from the specification, numbers come
 * from the nyan data at runtime (amounts, decay rates, costs); the constants
 * below are the ones the specification names or that it leaves open
 * (docs/aoe2-spec/LUECKEN.md, marked "vermutet").
 *
 * This header has no engine dependencies (host test gamestate/food_check.cpp).
 */

/// kind of a food source (how it is gathered)
enum class source_t : uint8_t {
	/// not a food source of W3 (berries, trees, mines: the plain gather rules)
	OTHER,
	/// prey (deer, class 9): flees when attacked, killed at a distance
	PREY,
	/// predator (boar, class 10): fights back and chases its attacker
	PREDATOR,
	/// herdable (sheep, turkey, class 58): changes owner by sight, killed in melee
	HERDABLE,
	/// farm (building, one farmer)
	FARM,
	/// fishing trap (building on water, one fishing ship)
	TRAP,
	/// fish (shore fish, ocean fish): gathered from the water by ships (shore fish also from land)
	FISH,
};

/// true for animals that have to be killed before their food can be gathered
inline bool is_animal(source_t kind) {
	return kind == source_t::PREY or kind == source_t::PREDATOR or kind == source_t::HERDABLE;
}

/// true for food buildings (farm, trap): one worker each, decay when empty
inline bool is_field(source_t kind) {
	return kind == source_t::FARM or kind == source_t::TRAP;
}

/// German label of a source for the info panel ("Schaf", "Feld")
inline const char *source_label(source_t kind) {
	switch (kind) {
	case source_t::PREY:
		return "Wild";
	case source_t::PREDATOR:
		return "Wildschwein";
	case source_t::HERDABLE:
		return "Herdentier";
	case source_t::FARM:
		return "Feld";
	case source_t::TRAP:
		return "Fischreuse";
	case source_t::FISH:
		return "Fisch";
	default:
		return "";
	}
}

/**
 * Kind of a food source from the GameEntity.types of its nyan object (short names of
 * hd_base.data.util.game_entity_type.types: AnimalPrey, AnimalPredator, Herdable,
 * ShoreFish, OceanFish) and its short entity name (Farm, FishingTrap).
 */
inline source_t source_of(const std::vector<std::string> &types, const std::string &name) {
	auto has = [&](const char *t) {
		return std::find(types.begin(), types.end(), std::string{t}) != types.end();
	};
	if (name == "FishingTrap") {
		return source_t::TRAP;
	}
	if (name == "Farm") {
		return source_t::FARM;
	}
	if (has("Herdable")) {
		return source_t::HERDABLE;
	}
	if (has("AnimalPrey")) {
		return source_t::PREY;
	}
	if (has("AnimalPredator")) {
		return source_t::PREDATOR;
	}
	if (has("ShoreFish") or has("OceanFish")) {
		return source_t::FISH;
	}
	return source_t::OTHER;
}

// ---------------------------------------------------------------- hunting

/// distance (tiles, edge to edge) from which a hunter strikes a prey: the spear (spec 02 §2.4, range 3)
constexpr double PREY_STRIKE_RANGE = 3.0;

/// melee distance (tiles, edge to edge) for boars and herdables
constexpr double MELEE_STRIKE_RANGE = 0.35;

/// strike range for a kind of animal
inline double strike_range(source_t kind) {
	return kind == source_t::PREY ? PREY_STRIKE_RANGE : MELEE_STRIKE_RANGE;
}

/// edge distance between two round units (centre distance minus both radii, at least 0)
inline double edge_distance(double ane, double ase, double ar, double bne, double bse, double br) {
	return std::max(0.0, std::hypot(ane - bne, ase - bse) - ar - br);
}

/// a hunter can strike: within the strike range of the kind (small slack for rounding)
inline bool can_strike(source_t kind, double edge) {
	return edge <= strike_range(kind) + 1e-6;
}

/// how far a prey runs when it is hit (tiles; spec 06 §6.6 "Fluchtpunkt", distance vermutet)
constexpr double FLEE_DISTANCE = 3.0;

/**
 * Flee point of a prey that is hit: away from the attacker, rotated by a random
 * offset (spec 06 §6.6), clamped to the map.
 *
 * @param jitter Random value in [-1, 1] (rotation up to ±40°).
 */
inline std::pair<double, double> flee_point(double ne, double se, double from_ne, double from_se,
                                            double distance, double jitter, double map_ne, double map_se) {
	double dx = ne - from_ne;
	double dy = se - from_se;
	double len = std::hypot(dx, dy);
	if (len < 1e-6) {
		dx = 1.0;
		dy = 0.0;
		len = 1.0;
	}
	const double angle = std::atan2(dy, dx) + std::clamp(jitter, -1.0, 1.0) * 0.698132;
	double x = ne + std::cos(angle) * distance;
	double y = se + std::sin(angle) * distance;
	x = std::clamp(x, 0.5, std::max(0.5, map_ne - 0.5));
	y = std::clamp(y, 0.5, std::max(0.5, map_se - 0.5));
	return {x, y};
}

// ---------------------------------------------------------------- decay

/**
 * Food left in a dead animal after dt seconds (spec 02 §2.4, L10): it decays with
 * the rate of the animal also while it is gathered.
 */
inline double decay(double amount, double rate, double dt) {
	if (not(rate > 0.0) or not(dt > 0.0)) {
		return std::max(0.0, amount);
	}
	return std::max(0.0, amount - rate * dt);
}

// ---------------------------------------------------------------- herdables

/// distance (tiles) within which a foreign unit takes over a herdable nobody of its owner sees
/// (spec 02 §2.4: "Standard-Aufgabe automatisch konvertieren, Sichtweite 2")
constexpr double HERD_CONVERT_RANGE = 2.0;

/// a unit that may see a herdable
struct Watcher {
	size_t player = 0;
	/// centre distance to the herdable (tiles)
	double distance = 0.0;
	/// line of sight of the unit (tiles)
	double sight = 0.0;
	/// tie breaker (entity id)
	uint64_t id = 0;
};

/**
 * Owner of a herdable after looking at the units around it (spec 02 §2.4):
 *  - a gaia herdable belongs to the player whose unit sees it (nearest unit, then
 *    lowest id) – "gehört dem Spieler, dessen Einheit sie zuerst sieht";
 *  - an owned herdable stays while a unit of its owner sees it; otherwise a foreign
 *    unit within HERD_CONVERT_RANGE takes it over (nearest first).
 * Herdables themselves are no watchers (the caller leaves them out).
 *
 * @return New owner, or nothing to keep the current owner.
 */
inline std::optional<size_t> herd_owner(size_t owner, bool owner_is_gaia, const std::vector<Watcher> &watchers) {
	const Watcher *best = nullptr;
	auto better = [](const Watcher &a, const Watcher *b) {
		return b == nullptr or a.distance < b->distance - 1e-9
		       or (std::abs(a.distance - b->distance) <= 1e-9 and a.id < b->id);
	};
	if (owner_is_gaia) {
		for (const auto &w : watchers) {
			if (w.distance <= w.sight + 1e-9 and better(w, best)) {
				best = &w;
			}
		}
		return best ? std::optional<size_t>{best->player} : std::nullopt;
	}
	for (const auto &w : watchers) {
		if (w.player == owner and w.distance <= w.sight + 1e-9) {
			return std::nullopt;
		}
	}
	for (const auto &w : watchers) {
		if (w.player != owner and w.distance <= HERD_CONVERT_RANGE + 1e-9 and better(w, best)) {
			best = &w;
		}
	}
	return best ? std::optional<size_t>{best->player} : std::nullopt;
}

// ---------------------------------------------------------------- farms

/// seconds a decayed farm (or trap) stays before it is removed (spec 02 §2.6: two timed decay stages; vermutet)
constexpr double FIELD_DECAY_SECONDS = 60.0;

/// state of a farm or trap
enum class field_t : uint8_t {
	/// foundation (being built, also after reseeding)
	BUILDING,
	/// producing (food left)
	GROWING,
	/// empty, waits for a rebuild until it is removed
	DECAYED,
};

/// what happens when a field runs empty
enum class empty_t : uint8_t {
	/// a queued reseed is used: the farmer rebuilds it at once
	RESEED,
	/// no reseed: it decays (the farmer looks for another free field)
	DECAY,
};

/// decision for a field that just ran empty (spec 13 L14: "erneuert sich nach dem Verfall selbst")
inline empty_t on_field_empty(source_t kind, size_t queued_reseeds, bool farmer_present) {
	if (kind == source_t::FARM and queued_reseeds > 0 and farmer_present) {
		return empty_t::RESEED;
	}
	return empty_t::DECAY;
}

/// a decayed field is removed after FIELD_DECAY_SECONDS
inline bool field_expired(double decayed_at, double now) {
	return now >= decayed_at + FIELD_DECAY_SECONDS - 1e-9;
}

/// terrain shown on the tiles of a farm per state and construction progress (index into the
/// construction overlays of the data: 0..n-1; GROWING = crops, DECAYED = harvested)
inline size_t construction_stage(double progress, size_t stages) {
	if (stages == 0) {
		return 0;
	}
	const double p = std::clamp(progress, 0.0, 1.0);
	auto idx = static_cast<size_t>(std::floor(p * static_cast<double>(stages)));
	return std::min(idx, stages - 1);
}

/// queue of the mill: "Feld neu einsäen" (spec 13 L14)
class ReseedQueue {
public:
	using cost_t = std::array<double, 4>;

	/// highest number of queued reseeds (spec: at least 3; vermutet like a training queue)
	static constexpr size_t MAX = 15;

	/// queue one reseed with its cost (paid by the caller); false if full
	bool push(const cost_t &cost) {
		if (this->items.size() >= MAX) {
			return false;
		}
		this->items.push_back(cost);
		return true;
	}

	/// cancel the reseed at index: its cost for the full refund
	std::optional<cost_t> cancel_at(size_t index) {
		if (index >= this->items.size()) {
			return std::nullopt;
		}
		auto cost = this->items[index];
		this->items.erase(this->items.begin() + static_cast<std::ptrdiff_t>(index));
		return cost;
	}

	/// use the oldest reseed (a field ran empty)
	std::optional<cost_t> take() {
		return this->cancel_at(0);
	}

	size_t size() const {
		return this->items.size();
	}

	bool empty() const {
		return this->items.empty();
	}

	const std::vector<cost_t> &get() const {
		return this->items;
	}

	void clear() {
		this->items.clear();
	}

private:
	std::vector<cost_t> items;
};

// ---------------------------------------------------------------- water buildings

/// terrain of a tile for a water building
enum class water_tile_t : uint8_t {
	/// outside the map
	OUTSIDE,
	/// placement terrain of the building (water / shallows of the dock, water of the trap)
	WATER,
	/// side terrain (beach, ice beach): the shore the dock needs next to it
	SHORE,
	/// other land
	LAND,
	/// other water (e.g. deep water for a dock)
	OTHER_WATER,
	/// an object or building is there
	BLOCKED,
	/// a unit (ship) is there
	OCCUPIED,
};

/// result of a water placement
enum class water_place_t : uint8_t {
	OK,
	OUTSIDE,
	/// a footprint tile is not the placement terrain
	NOT_WATER,
	/// no shore tile next to the footprint (dock)
	NO_SHORE,
	BLOCKED,
	OCCUPIED,
};

/// HUD message of a failed water placement
inline const char *water_place_message(water_place_t result) {
	switch (result) {
	case water_place_t::OK:
		return "";
	case water_place_t::OUTSIDE:
		return "Hier kann nicht gebaut werden (außerhalb der Karte)";
	case water_place_t::NOT_WATER:
		return "Hier kann nicht gebaut werden (nur auf Wasser)";
	case water_place_t::NO_SHORE:
		return "Hier kann nicht gebaut werden (nur am Ufer)";
	case water_place_t::BLOCKED:
		return "Hier kann nicht gebaut werden (belegt)";
	case water_place_t::OCCUPIED:
		return "Hier kann nicht gebaut werden (Einheiten im Weg)";
	default:
		return "Hier kann nicht gebaut werden";
	}
}

/**
 * Placement of a building on water (spec 04 §4.2 rule 6): every footprint tile has the
 * placement terrain, nothing blocks it; with \p need_shore at least one tile of the ring
 * around the footprint (8-neighbourhood) is shore. Order of the checks: outside, not
 * water, blocked, occupied, no shore.
 */
template <typename StateFn>
water_place_t check_water_placement(const std::vector<econ::tile_pos> &tiles, bool need_shore, StateFn &&state) {
	bool not_water = false;
	bool blocked = false;
	bool occupied = false;
	for (const auto &t : tiles) {
		switch (state(t)) {
		case water_tile_t::OUTSIDE:
			return water_place_t::OUTSIDE;
		case water_tile_t::WATER:
			break;
		case water_tile_t::BLOCKED:
			blocked = true;
			break;
		case water_tile_t::OCCUPIED:
			occupied = true;
			break;
		default:
			not_water = true;
			break;
		}
	}
	if (not_water) {
		return water_place_t::NOT_WATER;
	}
	if (blocked) {
		return water_place_t::BLOCKED;
	}
	if (occupied) {
		return water_place_t::OCCUPIED;
	}
	if (need_shore) {
		auto contains = [&](const econ::tile_pos &p) {
			return std::find(tiles.begin(), tiles.end(), p) != tiles.end();
		};
		for (const auto &t : tiles) {
			for (long dy = -1; dy <= 1; ++dy) {
				for (long dx = -1; dx <= 1; ++dx) {
					econ::tile_pos n{t.ne + dx, t.se + dy};
					if (not contains(n) and state(n) == water_tile_t::SHORE) {
						return water_place_t::OK;
					}
				}
			}
		}
		return water_place_t::NO_SHORE;
	}
	return water_place_t::OK;
}

// ---------------------------------------------------------------- info panel

/**
 * Info line of a food source for the selection panel ("Nahrung 140", "Feld – Nahrung 175",
 * "Wild – Nahrung 120 (verdirbt)").
 */
inline std::string amount_text(source_t kind, double amount, bool dead, bool decayed) {
	char buf[96];
	const long food = std::lround(std::floor(std::max(0.0, amount)));
	const char *label = source_label(kind);
	if (decayed) {
		std::snprintf(buf, sizeof(buf), "%s – verdorrt", label[0] ? label : "Feld");
	}
	else if (label[0] != '\0') {
		std::snprintf(buf, sizeof(buf), "%s – Nahrung %ld%s", label, food, dead ? " (verdirbt)" : "");
	}
	else {
		std::snprintf(buf, sizeof(buf), "Nahrung %ld", food);
	}
	return buf;
}

} // namespace openage::gamestate::food
