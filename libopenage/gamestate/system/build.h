// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>

#include "time/time.h"


namespace openage::gamestate {
class GameEntity;
class GameState;

namespace system {

/**
 * Constructing buildings (XR fork, production).
 *
 * A construction job is a small state machine in the Builder component, advanced
 * by the activity graph of villagers (gamestate/econ.h, branch BUILD):
 * walk next to the foundation -> build in steps of at most one second (each step
 * adds its share of the build time) -> idle when the building is complete.
 */
class Build {
public:
	/**
	 * Start a construction job from a build command.
	 *
	 * @return Runtime of the first step in simulation time.
	 */
	static const time::time_t build_command(const std::shared_ptr<gamestate::GameEntity> &entity,
	                                        const std::shared_ptr<openage::gamestate::GameState> &state,
	                                        const time::time_t &start_time);

	/**
	 * Advance the construction job of a game entity by one step.
	 *
	 * @return Runtime of the step in simulation time.
	 */
	static const time::time_t build_step(const std::shared_ptr<gamestate::GameEntity> &entity,
	                                     const std::shared_ptr<openage::gamestate::GameState> &state,
	                                     const time::time_t &start_time);

	/**
	 * Check if the game entity has an active construction job.
	 */
	static bool job_active(const std::shared_ptr<gamestate::GameEntity> &entity);
};

} // namespace system
} // namespace openage::gamestate
