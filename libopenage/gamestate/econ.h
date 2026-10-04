// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <optional>

#include <nyan/nyan.h>

#include "coord/phys.h"
#include "gamestate/types.h"
#include "time/time.h"


namespace openage {
namespace event {
class EventLoop;
}

namespace gamestate {
class GameEntity;
class GameState;

namespace activity {
class Activity;
}

/**
 * Economy of the XR fork: gathering, resource spots, drop sites.
 *
 * Kept apart from the entity factory and the command handling so that other
 * gameplay branches (combat) can be merged with few conflicts.
 */
namespace econ {

/**
 * Create the economy components of a game entity from its nyan abilities:
 * Gather (all Gather abilities + ResourceStorage capacities), Harvestable, DropSite.
 *
 * @param loop Event loop.
 * @param owner_db_view nyan view of the owner.
 * @param entity Game entity.
 * @param nyan_entity nyan game entity object name.
 */
void init_components(const std::shared_ptr<openage::event::EventLoop> &loop,
                     const std::shared_ptr<nyan::View> &owner_db_view,
                     const std::shared_ptr<GameEntity> &entity,
                     const nyan::fqon_t &nyan_entity);

/**
 * Activity graph for units that can gather (replaces the nyan "Unit" graph):
 *
 * Start -> Idle -> CheckQueue -(command)-> Branch -(move)-> Move -> Wait -(done)-> Idle
 *                     |                     |-(gather)-> GatherCommand -> GatherWait
 *                     v                     '-(other)-> drop command -> Idle
 *              WaitForCommand -> Branch
 * GatherWait -(step done)-> GatherActive? -(yes)-> GatherStep -> GatherWait
 *                                         '-(no)-> Idle
 * GatherWait / Wait -(new command)-> Branch
 *
 * @return Activity (shared by all gathering units).
 */
std::shared_ptr<activity::Activity> gather_activity();

/**
 * Find the resource entity under the cursor: the picking ray of the default
 * camera through the clicked ground point, tested against the vertical axis
 * (hitbox height) of every resource that is not empty.
 *
 * @param state Game state.
 * @param ground_hit Clicked point on the ground plane (up = 0), as sent by the input controller.
 * @param time Current time.
 *
 * @return Resource entity, if one is close to the ray.
 */
std::optional<entity_id_t> pick_resource(const std::shared_ptr<GameState> &state,
                                         const coord::phys3 &ground_hit,
                                         const time::time_t &time);

/**
 * Check if a game entity can gather the resource of another entity.
 */
bool can_gather_from(const std::shared_ptr<GameEntity> &gatherer,
                     const std::shared_ptr<GameEntity> &resource);

} // namespace econ
} // namespace gamestate
} // namespace openage
