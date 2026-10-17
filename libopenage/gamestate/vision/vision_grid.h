// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>


namespace openage::gamestate::vision {

/**
 * Sight grid of the fog of war (XR fork, rules in eigenen Worten nach den bekannten
 * Regeln des Genres: unerkundet / erkundet / sichtbar).
 *
 * One cell per map tile, index = ne + se * ne_size (the layout of the random map
 * generator). Per player:
 *  - a sight counter: how many sight circles cover the tile (> 0 = visible),
 *  - an explored flag: the tile was visible at least once (never cleared).
 *
 * Sight circles are stamped incrementally: an entity that moves to another tile
 * removes its circle at the old tile and adds it at the new one, so a tick costs
 * O(moved entities * circle area) instead of O(map). The result does not depend on
 * the order of the stamps (counters), so the grid is deterministic.
 *
 * This header has no engine dependencies (host test gamestate/vision/vision_check.cpp).
 */

/// state of a tile for one player
enum class tile_state_t : uint8_t {
	/// never seen: black
	UNEXPLORED = 0,
	/// seen before: terrain and the last seen buildings/resources in the fog
	EXPLORED = 1,
	/// in the line of sight of an own unit or building
	VISIBLE = 2,
};

/// how an entity is shown to a player
enum class display_t : uint8_t {
	/// drawn normally, can be picked
	NORMAL = 0,
	/// drawn darkened (in the fog: last seen building, resource on explored land)
	FOGGED = 1,
	/// not drawn, cannot be picked
	HIDDEN = 2,
};

/**
 * Display of an entity for a player (pure rule, see VisionState::tick):
 *  - own entities are always shown,
 *  - everything on a visible tile (any tile of its footprint) is shown,
 *  - on explored tiles (fog): resources and other static objects of gaia are shown
 *    darkened, foreign buildings only if the player saw them before (remembered),
 *    foreign units and animals (anything that moves) are hidden,
 *  - nothing is shown on unexplored tiles.
 *
 * @param own The entity belongs to the player.
 * @param movable The entity can move (units, animals).
 * @param gaia_static Static object of the neutral player (tree, mine, bush).
 * @param remembered Foreign building the player saw before.
 * @param best Best tile state over the footprint of the entity.
 */
inline display_t classify(bool own, bool movable, bool gaia_static, bool remembered, tile_state_t best) {
	if (own or best == tile_state_t::VISIBLE) {
		return display_t::NORMAL;
	}
	if (best == tile_state_t::UNEXPLORED or movable) {
		return display_t::HIDDEN;
	}
	return gaia_static or remembered ? display_t::FOGGED : display_t::HIDDEN;
}

/// line of sight radius in quarter tiles (key of the circle cache, max 64 tiles)
inline int quantize_radius(double radius) {
	return static_cast<int>(std::clamp<long>(std::lround(std::max(0.0, radius) * 4.0), 0L, 256L));
}

/// tile of a coordinate (floor; negative values stay negative)
inline int tile_of(double v) {
	return static_cast<int>(std::floor(v));
}

/**
 * Tiles of a sight circle: offsets (dne, dse) from the centre tile with
 * dne² + dse² <= (r + 0.5)², r = q / 4 (e.g. r = 4: 9 tiles across, as the
 * familiar line of sight of a villager).
 */
inline std::vector<std::pair<int, int>> make_disk(int q) {
	std::vector<std::pair<int, int>> offsets;
	const double r = static_cast<double>(q) / 4.0;
	const double limit = (r + 0.5) * (r + 0.5);
	const int n = static_cast<int>(std::ceil(r + 0.5));
	for (int ds = -n; ds <= n; ++ds) {
		for (int dn = -n; dn <= n; ++dn) {
			if (static_cast<double>(dn * dn + ds * ds) <= limit) {
				offsets.emplace_back(dn, ds);
			}
		}
	}
	return offsets;
}

/**
 * Visit the tiles of a footprint: tiles whose centre lies within \p radius (per
 * axis) of (ne, se), plus the tile of the point itself (same rule as
 * econ::footprint). Tiles outside of the map are skipped by the caller.
 */
template <class Fn>
inline void for_footprint(double ne, double se, double radius, Fn &&fn) {
	const double r = std::max(0.0, radius) + 1e-6;
	const int x0 = tile_of(ne - r - 0.5);
	const int x1 = tile_of(ne + r - 0.5) + 1;
	const int y0 = tile_of(se - r - 0.5);
	const int y1 = tile_of(se + r - 0.5) + 1;
	const int own_x = tile_of(ne);
	const int own_y = tile_of(se);
	bool has_own = false;
	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) {
			const double cx = static_cast<double>(x) + 0.5;
			const double cy = static_cast<double>(y) + 0.5;
			if (std::abs(cx - ne) <= r and std::abs(cy - se) <= r) {
				fn(x, y);
				has_own = has_own or (x == own_x and y == own_y);
			}
		}
	}
	if (not has_own) {
		fn(own_x, own_y);
	}
}

class VisionGrid {
public:
	VisionGrid() = default;

	/**
	 * @param ne_size Tiles along ne (Map::get_size()[0]).
	 * @param se_size Tiles along se (Map::get_size()[1]).
	 * @param players Number of players with a sight grid (gaia included is fine).
	 */
	VisionGrid(size_t ne_size, size_t se_size, size_t players) :
		ne_n{ne_size},
		se_n{se_size},
		n_players{players},
		counts(players, std::vector<uint16_t>(ne_size * se_size, 0)),
		explored_flags(players, std::vector<uint8_t>(ne_size * se_size, 0)),
		visible_n(players, 0),
		explored_n(players, 0) {}

	size_t ne_size() const {
		return this->ne_n;
	}
	size_t se_size() const {
		return this->se_n;
	}
	size_t players() const {
		return this->n_players;
	}
	size_t tiles() const {
		return this->ne_n * this->se_n;
	}

	bool inside(int ne, int se) const {
		return ne >= 0 and se >= 0 and static_cast<size_t>(ne) < this->ne_n and static_cast<size_t>(se) < this->se_n;
	}
	size_t index(int ne, int se) const {
		return static_cast<size_t>(ne) + static_cast<size_t>(se) * this->ne_n;
	}

	/// circle offsets for a quantized radius (cached)
	const std::vector<std::pair<int, int>> &disk(int q) {
		auto it = this->disks.find(q);
		if (it == this->disks.end()) {
			it = this->disks.emplace(q, make_disk(q)).first;
		}
		return it->second;
	}

	/**
	 * Add (\p delta = +1) or remove (-1) a sight circle of radius q / 4 around a tile.
	 * Removing a circle that was never added is a caller error (counters stay >= 0).
	 *
	 * @return Number of tiles that changed between visible and not visible.
	 */
	size_t stamp(size_t player, int ne, int se, int q, int delta) {
		if (player >= this->n_players or delta == 0) {
			return 0;
		}
		auto &count = this->counts[player];
		auto &explored = this->explored_flags[player];
		size_t changed = 0;
		for (const auto &[dn, ds] : this->disk(q)) {
			const int x = ne + dn;
			const int y = se + ds;
			if (not this->inside(x, y)) {
				continue;
			}
			const size_t i = this->index(x, y);
			if (delta > 0) {
				if (count[i] == 0) {
					changed += 1;
					this->visible_n[player] += 1;
				}
				if (count[i] < UINT16_MAX) {
					count[i] += 1;
				}
				if (explored[i] == 0) {
					explored[i] = 1;
					this->explored_n[player] += 1;
				}
			}
			else if (count[i] > 0) {
				count[i] -= 1;
				if (count[i] == 0) {
					changed += 1;
					this->visible_n[player] -= 1;
				}
			}
		}
		if (changed > 0) {
			this->rev += 1;
		}
		return changed;
	}

	bool visible(size_t player, int ne, int se) const {
		return player < this->n_players and this->inside(ne, se) and this->counts[player][this->index(ne, se)] > 0;
	}
	bool explored(size_t player, int ne, int se) const {
		return player < this->n_players and this->inside(ne, se)
		       and this->explored_flags[player][this->index(ne, se)] != 0;
	}
	tile_state_t state(size_t player, int ne, int se) const {
		if (this->visible(player, ne, se)) {
			return tile_state_t::VISIBLE;
		}
		return this->explored(player, ne, se) ? tile_state_t::EXPLORED : tile_state_t::UNEXPLORED;
	}

	/**
	 * Best state over the tiles of a footprint (radius 0: the tile of the point).
	 */
	tile_state_t footprint_state(size_t player, double ne, double se, double radius) const {
		auto best = tile_state_t::UNEXPLORED;
		for_footprint(ne, se, radius, [&](int x, int y) {
			auto s = this->state(player, x, y);
			if (static_cast<uint8_t>(s) > static_cast<uint8_t>(best)) {
				best = s;
			}
		});
		return best;
	}

	/// mark every tile explored ("Erkundet" at the start)
	void explore_all(size_t player) {
		if (player >= this->n_players) {
			return;
		}
		std::fill(this->explored_flags[player].begin(), this->explored_flags[player].end(), uint8_t{1});
		this->explored_n[player] = this->tiles();
		this->rev += 1;
	}

	/// tile states of a player (0 unexplored, 1 explored, 2 visible), index ne + se * ne_size
	void fill_states(size_t player, std::vector<uint8_t> &out) const {
		out.assign(this->tiles(), 0);
		if (player >= this->n_players) {
			return;
		}
		const auto &count = this->counts[player];
		const auto &explored = this->explored_flags[player];
		for (size_t i = 0; i < out.size(); ++i) {
			out[i] = count[i] > 0 ? 2 : explored[i] != 0 ? 1 : 0;
		}
	}

	/// changes of any counter between visible and not visible, or of the explored flags
	uint64_t revision() const {
		return this->rev;
	}
	size_t visible_count(size_t player) const {
		return player < this->n_players ? this->visible_n[player] : 0;
	}
	size_t explored_count(size_t player) const {
		return player < this->n_players ? this->explored_n[player] : 0;
	}
	/// sight counter of a tile (tests)
	uint16_t count(size_t player, int ne, int se) const {
		return player < this->n_players and this->inside(ne, se) ? this->counts[player][this->index(ne, se)] : 0;
	}

	/**
	 * Explored tiles as text for save games: "<ne>x<se>:<runs>", runs = lengths of
	 * alternating runs (unexplored first) over the tile index, comma separated,
	 * e.g. "4x4:5,2,9" = tiles 5 and 6 explored.
	 */
	std::string explored_text(size_t player) const {
		std::string out = std::to_string(this->ne_n) + "x" + std::to_string(this->se_n) + ":";
		if (player >= this->n_players) {
			return out + std::to_string(this->tiles());
		}
		const auto &explored = this->explored_flags[player];
		uint8_t current = 0;
		size_t run = 0;
		bool first = true;
		for (size_t i = 0; i < explored.size(); ++i) {
			const uint8_t v = explored[i] != 0 ? 1 : 0;
			if (v != current) {
				out += (first ? "" : ",") + std::to_string(run);
				first = false;
				current = v;
				run = 0;
			}
			run += 1;
		}
		out += (first ? "" : ",") + std::to_string(run);
		return out;
	}

	/**
	 * Restore the explored tiles from explored_text(). The sight counters are not
	 * touched (they follow the entities).
	 *
	 * @return false if the text is damaged or for another map size (nothing changed).
	 */
	bool set_explored_text(size_t player, const std::string &text) {
		if (player >= this->n_players) {
			return false;
		}
		const auto colon = text.find(':');
		const auto x = text.find('x');
		if (colon == std::string::npos or x == std::string::npos or x > colon) {
			return false;
		}
		char *end = nullptr;
		const unsigned long long w = std::strtoull(text.c_str(), &end, 10);
		const unsigned long long h = std::strtoull(text.c_str() + x + 1, &end, 10);
		if (w != this->ne_n or h != this->se_n) {
			return false;
		}
		std::vector<uint8_t> flags;
		flags.reserve(this->tiles());
		uint8_t current = 0;
		size_t pos = colon + 1;
		while (pos <= text.size()) {
			const auto comma = std::min(text.find(',', pos), text.size());
			const std::string part = text.substr(pos, comma - pos);
			if (part.empty() or part.find_first_not_of("0123456789") != std::string::npos) {
				return false;
			}
			const unsigned long long run = std::strtoull(part.c_str(), &end, 10);
			if (run > this->tiles() - flags.size()) {
				return false;
			}
			flags.insert(flags.end(), static_cast<size_t>(run), current);
			current = current != 0 ? 0 : 1;
			pos = comma + 1;
		}
		if (flags.size() != this->tiles()) {
			return false;
		}
		size_t n = 0;
		for (auto f : flags) {
			n += f;
		}
		this->explored_flags[player] = std::move(flags);
		this->explored_n[player] = n;
		this->rev += 1;
		return true;
	}

private:
	size_t ne_n = 0;
	size_t se_n = 0;
	size_t n_players = 0;
	std::vector<std::vector<uint16_t>> counts;
	std::vector<std::vector<uint8_t>> explored_flags;
	std::vector<size_t> visible_n;
	std::vector<size_t> explored_n;
	std::map<int, std::vector<std::pair<int, int>>> disks;
	uint64_t rev = 0;
};

} // namespace openage::gamestate::vision
