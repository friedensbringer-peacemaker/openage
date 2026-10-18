// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "gamestate/map_generator.h"
#include "gamestate/map_settings.h"


namespace openage::gamestate::vision {

/**
 * What a minimap draws for one entity (XR fork).
 */
enum class dot_kind_t : uint8_t {
	UNIT,
	BUILDING,
	TREE,
	GOLD,
	STONE,
	/// forage bushes and other static food
	FOOD,
	/// animals of gaia (deer, sheep)
	ANIMAL,
	OTHER,
};

/**
 * One entity on the minimap.
 */
struct MinimapDot {
	float ne = 0.0f;
	float se = 0.0f;
	/// half side of the footprint (tiles, 0 = a unit)
	float radius = 0.0f;
	dot_kind_t kind = dot_kind_t::OTHER;
	/// owner (player colours; gaia = the player after the last start)
	uint8_t player = 0;
	/// drawn in the fog (last seen)
	bool fogged = false;
	/// military unit (minimap mode "Militär")
	bool military = false;

	bool operator==(const MinimapDot &o) const {
		return ne == o.ne and se == o.se and radius == o.radius and kind == o.kind and player == o.player
		       and fogged == o.fogged and military == o.military;
	}
};

/**
 * What one player sees, published by the simulation after every vision tick
 * (thread-safe: immutable once published, shared by the renderer and the user
 * interface). Index of the tile arrays: ne + se * ne_size.
 */
struct Snapshot {
	/// counts every published snapshot (1 = the first)
	uint64_t version = 0;
	/// changes only when a tile state of the view player changed (fog texture uploads)
	uint64_t tiles_version = 0;
	vision_mode_t mode = vision_mode_t::ALL;
	uint64_t view_player = 0;
	/// gaia (dots in neutral colours)
	uint64_t neutral_player = 0;
	size_t ne_size = 0;
	size_t se_size = 0;
	double game_time = 0.0;
	/// tile states of the view player (0 unexplored, 1 explored, 2 visible); empty = all visible
	std::shared_ptr<const std::vector<uint8_t>> tiles{};
	/// moving entities (units, animals) that are not drawn normally (FOGGED / HIDDEN as display_t
	/// values); absent = NORMAL; rebuilt every tick
	std::unordered_map<uint32_t, uint8_t> display{};
	/// the same for entities that cannot move (trees, mines, buildings): shared between snapshots
	/// until the tiles or these entities change
	std::shared_ptr<const std::unordered_map<uint32_t, uint8_t>> static_display{};
	/// moving entities for the minimap (only what the view player may see), painted last
	std::vector<MinimapDot> dots{};
	/// entities that cannot move for the minimap, painted first
	std::shared_ptr<const std::vector<MinimapDot>> static_dots{};
	/// minimap ground colour per tile (RGBA as 0xAABBGGRR), static for a game
	std::shared_ptr<const std::vector<uint32_t>> terrain{};

	/// state of a tile (outside: unexplored; no tiles: visible)
	uint8_t tile(long ne, long se) const {
		if (not this->tiles or this->tiles->empty()) {
			return 2;
		}
		if (ne < 0 or se < 0 or static_cast<size_t>(ne) >= this->ne_size or static_cast<size_t>(se) >= this->se_size) {
			return 0;
		}
		return (*this->tiles)[static_cast<size_t>(ne) + static_cast<size_t>(se) * this->ne_size];
	}
	/// display of an entity as display_t value (0 = NORMAL)
	uint8_t display_of(uint32_t id) const {
		auto it = this->display.find(id);
		if (it != this->display.end()) {
			return it->second;
		}
		if (this->static_display) {
			auto st = this->static_display->find(id);
			if (st != this->static_display->end()) {
				return st->second;
			}
		}
		return 0;
	}
	/// all entities are drawn normally (no fog)
	bool all_normal() const {
		return this->display.empty() and (not this->static_display or this->static_display->empty());
	}
};

/// RGBA (0xAABBGGRR) from bytes
constexpr uint32_t rgba8(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
	return r | (g << 8) | (b << 16) | (a << 24);
}

/**
 * Minimap colour of a terrain kind of the random map generator (own palette in
 * the familiar tones: green land, brown dirt, blue water, light shallows).
 */
inline uint32_t terrain_color(map_terrain_t kind) {
	switch (kind) {
	case map_terrain_t::GRASS:
		return rgba8(74, 122, 58);
	case map_terrain_t::GRASS2:
		return rgba8(82, 128, 60);
	case map_terrain_t::GRASS3:
		return rgba8(92, 132, 64);
	case map_terrain_t::DIRT:
		return rgba8(140, 122, 78);
	case map_terrain_t::DIRT2:
		return rgba8(128, 112, 72);
	case map_terrain_t::DIRT3:
		return rgba8(150, 132, 88);
	case map_terrain_t::FOREST:
		return rgba8(52, 88, 44);
	case map_terrain_t::BEACH:
		return rgba8(200, 180, 120);
	case map_terrain_t::SHALLOWS:
		return rgba8(92, 140, 160);
	case map_terrain_t::WATER:
		return rgba8(48, 92, 156);
	case map_terrain_t::WATER_MEDIUM:
		return rgba8(40, 80, 140);
	case map_terrain_t::WATER_DEEP:
		return rgba8(30, 64, 120);
	case map_terrain_t::SAND:
		return rgba8(200, 168, 104);
	case map_terrain_t::SNOW:
		return rgba8(224, 230, 238);
	case map_terrain_t::SNOW_GRASS:
		return rgba8(184, 200, 184);
	case map_terrain_t::SNOW_DIRT:
		return rgba8(200, 192, 176);
	case map_terrain_t::SNOW_FOREST:
		return rgba8(160, 180, 168);
	case map_terrain_t::ICE:
		return rgba8(168, 200, 224);
	case map_terrain_t::COUNT:
	default:
		return rgba8(74, 122, 58);
	}
}

/**
 * Minimap ground colours of a generated map: terrain colour per tile, lightened
 * on slopes facing the top left and darkened on the others (the light of the
 * terrain shading), so hills stay recognisable.
 *
 * @param map Generated map (tiles, corners).
 *
 * @return RGBA per tile, index ne + se * width.
 */
inline std::vector<uint32_t> terrain_colors(const GeneratedMap &map) {
	std::vector<uint32_t> colors(map.width * map.height, rgba8(74, 122, 58));
	const bool corners = map.corners.size() == (map.width + 1) * (map.height + 1);
	for (size_t y = 0; y < map.height; ++y) {
		for (size_t x = 0; x < map.width; ++x) {
			const size_t i = x + y * map.width;
			uint32_t c = i < map.tiles.size() ? terrain_color(map.tiles[i]) : colors[i];
			if (corners) {
				auto h = [&](size_t cx, size_t cy) {
					return map.corners[cx + cy * (map.width + 1)];
				};
				// slope along ne (x) and se (y); light from -se (top left of the screen)
				const float d = (h(x, y) + h(x + 1, y) - h(x, y + 1) - h(x + 1, y + 1)) * 0.5f;
				const float shade = std::clamp(1.0f + 0.18f * d, 0.7f, 1.3f);
				auto ch = [&](int shift) {
					const float v = static_cast<float>((c >> shift) & 0xffu) * shade;
					return static_cast<uint32_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f) << shift;
				};
				c = ch(0) | ch(8) | ch(16) | (c & 0xff000000u);
			}
			colors[i] = c;
		}
	}
	return colors;
}

/**
 * Player colours of the minimap (same order as the sprites, world2d.frag.glsl:
 * blue, red, green, yellow, cyan, purple, grey, orange).
 */
inline uint32_t player_color(uint64_t player) {
	static constexpr std::array<uint32_t, 8> colors{
		rgba8(0, 76, 255),
		rgba8(255, 0, 0),
		rgba8(0, 255, 0),
		rgba8(255, 255, 0),
		rgba8(0, 255, 255),
		rgba8(255, 0, 255),
		rgba8(153, 153, 153),
		rgba8(255, 128, 0),
	};
	return colors[player % colors.size()];
}

} // namespace openage::gamestate::vision
