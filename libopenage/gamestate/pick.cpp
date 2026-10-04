// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "pick.h"

#include <cmath>
#include <limits>

#include "coord/scene.h"
#include "gamestate/combat/rules.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"


namespace openage::gamestate {

namespace {

combat::Point project(const Eigen::Matrix4f &camera, const coord::phys3 &pos) {
	auto world = pos.to_scene3().to_world_space();
	Eigen::Vector4f clip = camera * Eigen::Vector4f{world.x(), world.y(), world.z(), 1.0f};
	float w = std::abs(clip.w()) > 1e-6f ? clip.w() : 1.0f;
	return {clip.x() / w, clip.y() / w};
}

} // namespace


std::shared_ptr<GameEntity> pick_entity(const std::shared_ptr<GameState> &state,
                                        const time::time_t &time,
                                        const PickRequest &request,
                                        const pick_filter_t &filter) {
	std::shared_ptr<GameEntity> best;
	double best_score = std::numeric_limits<double>::max();

	const combat::Point click{request.ndc.x(), request.ndc.y()};
	const combat::Point ground{request.terrain.ne.to_double(), request.terrain.se.to_double()};

	for (const auto &[id, entity] : state->get_game_entities()) {
		auto shape = filter(entity);
		if (not shape) {
			continue;
		}
		auto pos = std::dynamic_pointer_cast<component::Position>(
			entity->get_component(component::component_t::POSITION));
		auto anchor = pos->get_positions().get(time);

		double score = 0.0;
		if (request.camera) {
			const auto &camera = *request.camera;
			auto base = project(camera, anchor);
			auto top = project(camera, anchor + coord::phys3_delta{0, 0, shape->height});
			// projected radius: the larger of the two ground axes (isometric view)
			auto side_ne = project(camera, anchor + coord::phys3_delta{shape->radius, 0, 0});
			auto side_se = project(camera, anchor + coord::phys3_delta{0, shape->radius, 0});
			double width = std::max(combat::distance(base, side_ne), combat::distance(base, side_se));
			width = std::max(width, 1e-4) + request.slack;
			double dist = combat::point_segment_distance(click, base, top);
			if (dist > width) {
				continue;
			}
			score = dist / width;
		}
		else {
			double dist = combat::distance(ground, {anchor.ne.to_double(), anchor.se.to_double()});
			double limit = shape->radius + 0.25 + request.slack;
			if (dist > limit) {
				continue;
			}
			score = dist / limit;
		}

		if (score < best_score) {
			best_score = score;
			best = entity;
		}
	}
	return best;
}

} // namespace openage::gamestate
