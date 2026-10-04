// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <vector>

#include "coord/phys.h"
#include "event/eventhandler.h"
#include "gamestate/component/internal/commands/types.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage::gamestate {
class GameState;

namespace combat {

/**
 * Combat part of the send command handler (XR fork).
 *
 *  - drops ids of entities that are dead or already removed
 *  - ATTACK: the entities that can attack attack "target_entity" (param,
 *    entity_id_t) or the enemy under the cursor
 *  - MOVE (right click): if an enemy of the commanded entities' owner is under
 *    the cursor (pick, see below), the entities that can attack attack it
 *    instead of moving; all others stop attacking and move
 *  - IDLE: stop attacking
 *
 * Picking uses the params "camera_matrix" (Eigen::Matrix4f, projection *
 * view) and "pick_ndc" (Eigen::Vector2f, cursor in NDC) if the input sent
 * them, else the distance on the ground to \p target.
 *
 * @param state Game state.
 * @param time Time of the command.
 * @param type Command type.
 * @param ids Commanded entities; on return only those that still need the
 *            regular command (move/idle) of the handler.
 * @param target Target point on the terrain.
 * @param params Event parameters.
 */
void handle_command(const std::shared_ptr<GameState> &state,
                    const time::time_t &time,
                    component::command::command_t type,
                    std::vector<entity_id_t> &ids,
                    const coord::phys3 &target,
                    const openage::event::EventHandler::param_map &params);

} // namespace combat
} // namespace openage::gamestate
