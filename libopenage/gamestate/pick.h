// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <eigen3/Eigen/Dense>

#include "coord/phys.h"
#include "time/time.h"


namespace openage::gamestate {
class GameEntity;
class GameState;

/**
 * Picking of game entities under the cursor (XR fork).
 *
 * Shared by the context commands of a right click (attack an enemy, gather a
 * resource, ...): the caller decides with a filter which entities count and
 * how large they are.
 */

/**
 * Size of a pickable entity.
 */
struct PickShape {
	/// radius in the ground plane (tiles), e.g. the Collision hitbox
	double radius = 0.5;
	/// height of the sprite above the anchor point (tiles); 0 = flat
	double height = 1.0;
};

/**
 * Click to pick at.
 */
struct PickRequest {
	/// point on the terrain under the cursor (Map::pick_terrain of the plane hit)
	coord::phys3 terrain{0, 0, 0};
	/// projection * view matrix of the camera; if set, entities are picked in
	/// screen space (sprites stand upright), else by distance on the ground
	std::optional<Eigen::Matrix4f> camera{};
	/// cursor in normalized device coordinates (with camera)
	Eigen::Vector2f ndc{0.0f, 0.0f};
	/// extra tolerance on the ground (tiles, without camera) or in NDC (with camera)
	double slack = 0.0;
};

/**
 * Filter: shape of a pickable entity, or nothing if the entity does not count.
 */
using pick_filter_t = std::function<std::optional<PickShape>(const std::shared_ptr<GameEntity> &)>;

/**
 * Find the entity under the cursor.
 *
 * Screen space (camera set): the sprite is approximated as a segment from the
 * anchor point to \p height above it with the projected \p radius as width;
 * the entity whose segment is closest relative to its width wins.
 * Ground (no camera): the entity with the smallest distance to the terrain
 * point within its radius + 0.25 tiles wins.
 *
 * @param state Game state (simulation thread only).
 * @param time Simulation time.
 * @param request Click.
 * @param filter Which entities count and their shape.
 *
 * @return Picked entity or nullptr.
 */
std::shared_ptr<GameEntity> pick_entity(const std::shared_ptr<GameState> &state,
                                        const time::time_t &time,
                                        const PickRequest &request,
                                        const pick_filter_t &filter);

/**
 * Find the entity under the cursor along the view ray of the default camera
 * (renderer::camera::CAM_DIRECTION) through the clicked ground point, in world
 * units: every entity is a vertical segment from its anchor to \p height
 * above it; the entity whose segment is closest to the ray within its
 * \p radius wins. Needs no camera matrix (used for resources, see econ.h).
 *
 * @param state Game state (simulation thread only).
 * @param time Simulation time.
 * @param ground_hit Clicked point on the ground plane (up = 0), as sent by the input.
 * @param filter Which entities count; radius and height in world units.
 *
 * @return Picked entity or nullptr.
 */
std::shared_ptr<GameEntity> pick_entity_ray(const std::shared_ptr<GameState> &state,
                                            const time::time_t &time,
                                            const coord::phys3 &ground_hit,
                                            const pick_filter_t &filter);

} // namespace openage::gamestate
