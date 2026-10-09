// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "gamestate/save_format.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage {
namespace event {
class EventLoop;
}

namespace gamestate {
class EntityFactory;
class GameState;

namespace prod {
class Production;
}

/**
 * Save games of the XR fork (format: gamestate/save_format.h).
 *
 * Saving captures the game state in the simulation thread (capture()) and
 * writes the text file. Loading starts a new engine with the map settings of
 * the file (the map is generated again, deterministically) and restore()
 * applies the saved state right after the map exists, before the simulation
 * loop runs:
 *
 *  - players: resource stockpiles
 *  - generated map objects (same ids as in the file): position, health,
 *    remaining resources; objects that are gone are removed and their tiles
 *    freed
 *  - everything else (trained units, placed buildings, test units): created
 *    again with new ids (the file ids are mapped), position, health, carried
 *    resources, construction progress (foundation, tiles blocked), training
 *    queue with the time left, rally point
 *  - orders: gather, build, attack and move orders are given again (the
 *    units walk to their target once more); idle units stay idle
 *
 * Not stored (documented losses): the camera and selection, the decisions of
 * the computer opponent (it starts again at the saved game time, so its
 * timers such as the first attack still count from the game start), projectiles
 * and death animations (dying entities are saved as removed), the exact
 * progress of a gather step and of the current walk.
 */
namespace save {

/// what restore() did (for logs and checks)
struct RestoreResult {
	bool ok = false;
	std::string error;
	size_t updated = 0;
	size_t created = 0;
	size_t removed = 0;
	size_t orders = 0;
	size_t skipped = 0;
};

/**
 * Capture the game state (simulation thread only).
 *
 * @param state Game state.
 * @param map Map settings of the running game.
 * @param generated Entity id range of the generated map objects (Game::get_generated_entity_range()).
 * @param time Current simulation time.
 * @param title Title for the slot list.
 */
SaveData capture(const std::shared_ptr<GameState> &state,
                 const MapSettings &map,
                 std::pair<entity_id_t, entity_id_t> generated,
                 const time::time_t &time,
                 const std::string &title);

/**
 * Apply a save game to a freshly generated map (simulation thread only, before
 * the loop runs). The map settings of the game must be those of the file.
 *
 * @param data Save game.
 * @param state Game state with the generated map.
 * @param loop Event loop of the game.
 * @param factory Entity factory (new entities).
 * @param production Production (foundations are tracked, status message).
 * @param generated Entity id range of the generated map objects.
 * @param time Current simulation time (the saved game time).
 */
RestoreResult restore(const SaveData &data,
                      const std::shared_ptr<GameState> &state,
                      const std::shared_ptr<openage::event::EventLoop> &loop,
                      const std::shared_ptr<EntityFactory> &factory,
                      const std::shared_ptr<prod::Production> &production,
                      std::pair<entity_id_t, entity_id_t> generated,
                      const time::time_t &time);

} // namespace save
} // namespace gamestate
} // namespace openage
