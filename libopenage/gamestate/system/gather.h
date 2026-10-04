// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>

#include "time/time.h"


namespace openage::gamestate {
class GameEntity;
class GameState;

namespace system {

/**
 * Gathering resources (XR fork, economy).
 *
 * A gather job is a small state machine in the Gather component, advanced by
 * the activity graph of gathering units (gamestate/econ.h):
 * walk to the resource -> gather in chunks until full -> walk to the nearest
 * drop site of the owner -> drop off (player resources +) -> walk back.
 * An empty resource is removed from the world and the unit continues with the
 * nearest resource of the same type, or goes idle.
 */
class Gather {
public:
	/**
	 * Start a gather job from a gather command.
	 *
	 * @param entity Game entity.
	 * @param state Game state.
	 * @param start_time Start time of change.
	 *
	 * @return Runtime of the first step in simulation time.
	 */
	static const time::time_t gather_command(const std::shared_ptr<gamestate::GameEntity> &entity,
	                                         const std::shared_ptr<openage::gamestate::GameState> &state,
	                                         const time::time_t &start_time);

	/**
	 * Advance the gather job of a game entity by one step.
	 *
	 * @param entity Game entity.
	 * @param state Game state.
	 * @param start_time Start time of change.
	 *
	 * @return Runtime of the step in simulation time.
	 */
	static const time::time_t gather_step(const std::shared_ptr<gamestate::GameEntity> &entity,
	                                      const std::shared_ptr<openage::gamestate::GameState> &state,
	                                      const time::time_t &start_time);

	/**
	 * Check if the game entity has an active gather job.
	 */
	static bool job_active(const std::shared_ptr<gamestate::GameEntity> &entity);
};

} // namespace system
} // namespace openage::gamestate
