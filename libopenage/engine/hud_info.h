// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>


namespace openage::engine {

/**
 * State of the match from the view of one player (XR fork, mirrors
 * gamestate::combat::match_state_t without pulling in the game state).
 */
enum class hud_match_t {
	RUNNING,
	VICTORY,
	DEFEAT,
	DRAW,
};

/**
 * One selected entity for a HUD (XR fork).
 */
struct HudEntity {
	uint64_t id = 0;
	/// short nyan name of the game entity type, e.g. "Villager", "TownCenter"
	std::string name;
	/// GameEntity.types of the nyan object
	bool unit = false;
	bool building = false;
	bool villager = false;
	/// ranged attack (ShootProjectile)
	bool ranged = false;
	/// herdables, prey and ambient objects (sheep, trees, mines)
	bool neutral_object = false;
	int64_t health = 0;
	/// 0 = no health bar
	int64_t max_health = 0;
	bool alive = false;
};

/**
 * Snapshot of everything a HUD shows for one player (XR fork).
 *
 * Plain data without engine, Qt or GL types, so embedders can copy it between
 * threads and test their HUD code without the engine.
 */
struct HudInfo {
	/// the game exists (false while the engine is starting or after it stopped)
	bool game = false;
	/// the player exists in the game
	bool player = false;
	/// stockpile: food, wood, gold, stone (gamestate::resource_t order)
	std::array<double, 4> resources{};
	/// own units (without buildings) left; nothing until the first combat scan
	/// (one simulated second after the start)
	std::optional<size_t> units{};
	/// own units + buildings left; nothing until the first combat scan
	std::optional<size_t> units_and_buildings{};
	/// selected entities that are still in the game
	std::vector<uint64_t> selected{};
	/// first entity of the selection (if there is one)
	std::optional<HudEntity> first{};
	/// match state of the player
	hud_match_t match = hud_match_t::RUNNING;
	/// simulation time (s) when the match was decided (only if match != RUNNING)
	double decided_at = 0.0;
};

} // namespace openage::engine
