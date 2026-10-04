// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gamestate/econ_math.h"
#include "gamestate/resources.h"


namespace openage::gamestate::prod {

/**
 * Rules of production and construction (XR fork): which units and buildings are
 * offered, their HUD codes and labels, foundation placement, the training queue
 * and the population limit.
 *
 * This header has no engine dependencies, so it can be tested without the engine
 * (gamestate/production_check.cpp).
 */

/// maximum length of a training queue (AoE II: 5 per click, 15 per building; we keep it short)
constexpr size_t MAX_QUEUE = 5;

/// population limit of a player
constexpr size_t POPULATION_LIMIT = 200;

/// building time if the data has none (seconds)
constexpr double DEFAULT_BUILD_TIME = 25.0;

/// training time if the data has none (seconds)
constexpr double DEFAULT_TRAIN_TIME = 25.0;

/**
 * Units and buildings offered in the HUD.
 *
 * The data of the converted HD edition has more (eagle warriors, castles, wonders, ...),
 * most of them behind ages and technologies that do not exist yet. Until then the
 * Dark/Feudal age basics are offered, independent of their unlock conditions.
 */
struct KnownEntry {
	/// last part of the game entity name in nyan ("Villager" for ...villager.Villager)
	const char *name;
	/// German label for the HUD
	const char *label;
	/// HUD command code (stable; 1xx = train, 2xx = build)
	int code;
	/// true: built by villagers, false: trained in a building
	bool building;
	/// icon hint for the HUD ("villager", "house", "sword", "bow", "horse", "hammer")
	const char *icon;
};

constexpr std::array<KnownEntry, 13> KNOWN{{
	{"Villager", "Dorfbewohner", 101, false, "villager"},
	{"Militia", "Milizsoldat", 102, false, "sword"},
	{"Spearman", "Speerkämpfer", 103, false, "sword"},
	{"Archer", "Bogenschütze", 104, false, "bow"},
	{"Skirmisher", "Plänkler", 105, false, "bow"},
	{"ScoutCavalry", "Späher", 106, false, "horse"},
	{"House", "Haus", 201, true, "house"},
	{"Mill", "Mühle", 202, true, "hammer"},
	{"LumberCamp", "Holzfällerlager", 203, true, "hammer"},
	{"MiningCamp", "Bergbaulager", 204, true, "hammer"},
	{"Barracks", "Kaserne", 205, true, "sword"},
	{"ArcheryRange", "Bogenschießstand", 206, true, "bow"},
	{"Stable", "Stall", 207, true, "horse"},
}};

/// labels of other game entities that show up in the selection
constexpr std::array<std::pair<const char *, const char *>, 4> OTHER_LABELS{{
	{"TownCenter", "Dorfzentrum"},
	{"Knight", "Ritter"},
	{"Monk", "Mönch"},
	{"Castle", "Burg"},
}};

/// short name of a nyan game entity ("hd_base...house.House" -> "House")
inline std::string short_name(const std::string &fqon) {
	auto dot = fqon.rfind('.');
	return dot == std::string::npos ? fqon : fqon.substr(dot + 1);
}

/// entry of a known unit or building (nullptr if not offered)
inline const KnownEntry *known(const std::string &name) {
	for (const auto &entry : KNOWN) {
		if (name == entry.name) {
			return &entry;
		}
	}
	return nullptr;
}

/// entry by HUD code (nullptr if unknown)
inline const KnownEntry *known(int code) {
	for (const auto &entry : KNOWN) {
		if (code == entry.code) {
			return &entry;
		}
	}
	return nullptr;
}

/// "ArcheryRange" -> "Archery Range"
inline std::string split_camel(const std::string &name) {
	std::string out;
	for (size_t i = 0; i < name.size(); ++i) {
		auto c = static_cast<unsigned char>(name[i]);
		if (i > 0 and std::isupper(c) and not std::isupper(static_cast<unsigned char>(name[i - 1]))) {
			out += ' ';
		}
		out += name[i];
	}
	return out;
}

/// HUD label of a game entity short name (German if known)
inline std::string label_of(const std::string &name) {
	if (const auto *entry = known(name)) {
		return entry->label;
	}
	for (const auto &[n, label] : OTHER_LABELS) {
		if (name == n) {
			return label;
		}
	}
	return split_camel(name);
}

/// order of the offered entries (position in KNOWN), unknown entries last
inline size_t display_order(const std::string &name) {
	for (size_t i = 0; i < KNOWN.size(); ++i) {
		if (name == KNOWN[i].name) {
			return i;
		}
	}
	return KNOWN.size();
}

/// tiles per side of a building footprint (hitbox radius 1.0 -> 2 tiles, 2.0 -> 4)
inline long footprint_side(double radius) {
	return std::max(1L, static_cast<long>(std::lround(2.0 * std::max(0.0, radius))));
}

/**
 * Anchor of a building placed at a clicked point: the centre of its footprint snapped
 * to the tile grid (even sizes on a tile corner, odd sizes on a tile centre).
 */
inline std::pair<double, double> snap_anchor(double ne, double se, double radius) {
	const long side = footprint_side(radius);
	if (side % 2 == 0) {
		return {std::round(ne), std::round(se)};
	}
	return {std::floor(ne) + 0.5, std::floor(se) + 0.5};
}

/// tiles covered by a building at an anchor (same rule as econ::footprint)
inline std::vector<econ::tile_pos> building_tiles(double ne, double se, double radius) {
	// radius of the snapped footprint: half the side, so that rounding cannot add a row
	const double r = 0.5 * static_cast<double>(footprint_side(radius)) - 0.25;
	return econ::footprint(ne, se, r);
}

/// state of a tile for placing a foundation
enum class tile_state_t {
	FREE,
	OUTSIDE,
	WATER,
	/// tree, mine, building, cliff
	BLOCKED,
	/// a unit stands there
	OCCUPIED,
};

/// result of the placement check
enum class placement_t {
	OK,
	OUTSIDE,
	WATER,
	BLOCKED,
	OCCUPIED,
};

/// HUD message of a failed placement
inline const char *placement_message(placement_t result) {
	switch (result) {
	case placement_t::OK:
		return "";
	case placement_t::OUTSIDE:
		return "Hier kann nicht gebaut werden (außerhalb der Karte)";
	case placement_t::WATER:
		return "Hier kann nicht gebaut werden (Wasser)";
	case placement_t::BLOCKED:
		return "Hier kann nicht gebaut werden (belegt)";
	case placement_t::OCCUPIED:
		return "Hier kann nicht gebaut werden (Einheiten im Weg)";
	default:
		return "Hier kann nicht gebaut werden";
	}
}

/**
 * Check all tiles of a footprint. The first problem in the order outside, water,
 * blocked, occupied wins.
 *
 * @param tiles Footprint.
 * @param state Function tile_pos -> tile_state_t.
 */
template <typename StateFn>
placement_t check_placement(const std::vector<econ::tile_pos> &tiles, StateFn &&state) {
	bool water = false;
	bool blocked = false;
	bool occupied = false;
	for (const auto &tile : tiles) {
		switch (state(tile)) {
		case tile_state_t::OUTSIDE:
			return placement_t::OUTSIDE;
		case tile_state_t::WATER:
			water = true;
			break;
		case tile_state_t::BLOCKED:
			blocked = true;
			break;
		case tile_state_t::OCCUPIED:
			occupied = true;
			break;
		default:
			break;
		}
	}
	if (water) {
		return placement_t::WATER;
	}
	if (blocked) {
		return placement_t::BLOCKED;
	}
	if (occupied) {
		return placement_t::OCCUPIED;
	}
	return placement_t::OK;
}

/// German name of a resource for messages
inline const char *resource_label(resource_t type) {
	switch (type) {
	case resource_t::FOOD:
		return "Nahrung";
	case resource_t::WOOD:
		return "Holz";
	case resource_t::GOLD:
		return "Gold";
	case resource_t::STONE:
		return "Stein";
	default:
		return "?";
	}
}

/// first resource that is missing for a cost (nothing if affordable)
inline std::optional<resource_t> missing_resource(const resource_amounts_t &stock, const resource_amounts_t &cost) {
	for (size_t i = 0; i < RESOURCE_COUNT; ++i) {
		if (cost[i] > stock[i] + 1e-9) {
			return static_cast<resource_t>(i);
		}
	}
	return std::nullopt;
}

/// "Nicht genug Holz"
inline std::string missing_message(resource_t type) {
	return std::string{"Nicht genug "} + resource_label(type);
}

/// population limit: provided housing, at most POPULATION_LIMIT
inline size_t population_cap(double provided) {
	if (not(provided > 0.0)) {
		return 0;
	}
	return std::min(POPULATION_LIMIT, static_cast<size_t>(std::floor(provided + 1e-9)));
}

/// construction progress after a building step of one builder (fraction 0..1)
inline double build_progress(double progress, double step_seconds, double build_time) {
	if (not(build_time > 0.0)) {
		return 1.0;
	}
	return std::clamp(progress + std::max(0.0, step_seconds) / build_time, 0.0, 1.0);
}

/// health of a building under construction: rises with the progress, at least 1 (AoE II)
inline long long construction_health(long long max_health, double progress) {
	if (max_health <= 0) {
		return 0;
	}
	auto value = static_cast<long long>(std::lround(static_cast<double>(max_health) * std::clamp(progress, 0.0, 1.0)));
	return std::clamp(value, 1LL, max_health);
}

/**
 * Training queue of a building.
 *
 * The first item starts when a population slot is free (it reserves the slot) and is
 * done after its training time; the next item starts at that moment. Costs are paid
 * by the caller when an item is added and refunded when it is cancelled.
 */
class TrainQueue {
public:
	struct Item {
		/// short name of the game entity
		std::string name;
		/// nyan game entity
		std::string fqon;
		resource_amounts_t cost{};
		/// training time (seconds)
		double time = DEFAULT_TRAIN_TIME;
		/// start time (seconds), negative while waiting
		double started = -1.0;
	};

	/// add an item, false if the queue is full
	bool push(const Item &item) {
		if (this->items.size() >= MAX_QUEUE) {
			return false;
		}
		this->items.push_back(item);
		this->items.back().started = -1.0;
		return true;
	}

	/// remove the last item (its cost is refunded by the caller)
	std::optional<Item> cancel_last() {
		if (this->items.empty()) {
			return std::nullopt;
		}
		auto item = this->items.back();
		this->items.pop_back();
		if (this->items.empty()) {
			this->waiting = false;
		}
		return item;
	}

	/**
	 * Advance to a time.
	 *
	 * @param now Current time (seconds).
	 * @param free_slots Free population slots of the owner; reduced for every item that starts.
	 *
	 * @return Items done (their units must be spawned), in order.
	 */
	std::vector<Item> advance(double now, size_t &free_slots) {
		std::vector<Item> done;
		double start_at = now;
		while (not this->items.empty()) {
			auto &head = this->items.front();
			if (head.started < 0.0) {
				if (free_slots == 0) {
					this->waiting = true;
					break;
				}
				free_slots -= 1;
				head.started = start_at;
				this->waiting = false;
			}
			const double end = head.started + std::max(0.0, head.time);
			if (now + 1e-9 < end) {
				break;
			}
			done.push_back(head);
			this->items.pop_front();
			// the next item starts when this one is done
			start_at = end;
		}
		return done;
	}

	/// progress of the first item (0..1)
	double progress(double now) const {
		if (this->items.empty() or this->items.front().started < 0.0) {
			return 0.0;
		}
		const auto &head = this->items.front();
		if (not(head.time > 0.0)) {
			return 1.0;
		}
		return std::clamp((now - head.started) / head.time, 0.0, 1.0);
	}

	/// true if the first item waits for a population slot
	bool is_waiting() const {
		return this->waiting and not this->items.empty();
	}

	/// 1 if the first item reserves a population slot
	size_t reserved() const {
		return (not this->items.empty() and this->items.front().started >= 0.0) ? 1 : 0;
	}

	const std::deque<Item> &get_items() const {
		return this->items;
	}

	size_t size() const {
		return this->items.size();
	}

private:
	std::deque<Item> items;
	bool waiting = false;
};

} // namespace openage::gamestate::prod
