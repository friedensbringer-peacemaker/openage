// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "coord/phys.h"
#include "coord/tile.h"
#include "gamestate/building_math.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage::gamestate {
class GameEntity;
class GameState;

namespace building {

/// nyan short names of wall segments (spec 04 §4.4: 1x1 buildings that connect)
inline bool is_wall_name(const std::string &name) {
	return name == "PalisadeWall" or name == "StoneWall" or name == "FortifiedWall";
}

/// nyan short name of the gate
inline bool is_gate_name(const std::string &name) {
	return name == "StoneGate";
}

/// a gate on the map (spec 04 §4.4, decision L32)
struct Gate {
	player_id_t owner = 0;
	gate_dir_t dir = gate_dir_t::EAST_WEST;
	/// first of its GATE_LENGTH tiles
	tile_t anchor{0, 0};
	/// locked: blocks everyone (also the owner)
	bool locked = false;
	/// the gate is built (a foundation blocks its tiles like any foundation)
	bool finished = false;

	std::vector<tile_t> tiles() const {
		return gate_tiles(this->anchor.first, this->anchor.second, this->dir);
	}
};

/// a status message for the HUD of a player (repairs that stop, gates)
struct Message {
	player_id_t player = 0;
	std::string text;
	bool warn = false;
};

/**
 * Buildings of round W5 (docs/aoe2-spec 04): gates and their passage, the build
 * speed of the players and messages of the build system for the HUD.
 *
 *  - Gates do not block their tiles in the path grid while they are open, so the
 *    pathfinder routes own units through them; paths of other players' units are
 *    cut in front of a gate (system/move.cpp, clip_path). A locked gate blocks
 *    its tiles for everyone (decision L32).
 *  - The footprint of a gate is its line of 4 tiles (the hitbox of the data is a
 *    4x1 rectangle); the combat state frees exactly these tiles when it dies.
 *  - Build speed k (player resource 195, spec 04 §4.3): the converted data carry no
 *    such resource (the Treadmill Crane has no updates), so it is 1 unless set.
 *
 * Runs in the simulation thread; is_locked_shared() may be called from any thread.
 */
class BuildingState : public std::enable_shared_from_this<BuildingState> {
public:
	BuildingState() = default;
	~BuildingState() = default;

	/// hooks into the combat state (footprint of gates, death of gates); once per game
	void start(const std::shared_ptr<GameState> &state);

	// ---- build speed (spec 04 §4.3)

	/// build speed k of a player (1 if not set)
	double build_speed(player_id_t player) const;
	/// set k (tests, technologies)
	void set_build_speed(player_id_t player, double k);

	// ---- gates

	/// a gate foundation was placed (or loaded): its tiles and orientation are known
	void add_gate(entity_id_t id, const Gate &gate);
	/// the gate is built: open unless locked (frees its tiles in the path grid)
	void gate_finished(const std::shared_ptr<GameState> &state, entity_id_t id, const time::time_t &time);
	/// gate of an entity, nullptr if it is none
	const Gate *gate(entity_id_t id) const;
	/// all gates (sorted by id)
	const std::map<entity_id_t, Gate> &gates() const;

	/**
	 * Lock or unlock a gate of \p player (HD command "Tor verriegeln"): a locked gate blocks its tiles in the
	 * path grid for everyone, an unlocked finished gate frees them again.
	 *
	 * @return false if \p id is no gate of the player.
	 */
	bool set_locked(const std::shared_ptr<GameState> &state,
	                entity_id_t id,
	                player_id_t player,
	                bool locked,
	                const time::time_t &time);

	/// a unit of \p mover may not cross \p tile (gate of another player, or locked)
	bool blocks(player_id_t mover, long ne, long se) const;

	/**
	 * Cut a path in front of the first gate tile that \p mover may not cross (sampled every
	 * quarter tile). Tiles at the start of the path (the unit stands on a gate) are allowed.
	 *
	 * @return the waypoints up to the last free sample point, unchanged if nothing blocks.
	 */
	std::vector<coord::phys3> clip_path(player_id_t mover, const std::vector<coord::phys3> &waypoints) const;

	/// footprint of a special building (gates: their tiles), nothing for all other entities
	std::optional<std::vector<coord::tile>> footprint(entity_id_t id) const;

	/// thread-safe: the gate is locked (HUD), nothing if \p id is no gate
	std::optional<bool> is_locked_shared(entity_id_t id) const;

	/// a gate died or was removed (combat hook, save games)
	void forget(entity_id_t id);

	// ---- messages for the HUD (production drains them)

	void post(player_id_t player, const std::string &text, bool warn);
	std::vector<Message> take_messages();

private:
	void publish();

	bool started = false;
	std::unordered_map<player_id_t, double> speed;
	std::map<entity_id_t, Gate> gate_list;
	/// tile -> gate entity
	std::map<std::pair<long, long>, entity_id_t> gate_tiles_map;
	std::vector<Message> messages;

	mutable std::mutex shared_mutex;
	std::unordered_map<entity_id_t, bool> locked_shared;
};

} // namespace building
} // namespace openage::gamestate
