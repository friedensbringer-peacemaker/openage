// Copyright 2023-2026 the openage authors. See copying.md for legal info.

#include "drag_select.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <eigen3/Eigen/Dense>

#include "coord/phys.h"
#include "coord/pixel.h"
#include "coord/scene.h"
#include "curve/discrete.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/pick.h"
#include "gamestate/selection_rules.h"
#include "gamestate/types.h"


namespace openage::gamestate::event {

DragSelectHandler::DragSelectHandler() :
	OnceEventHandler{"game.drag_select"} {}

void DragSelectHandler::setup_event(const std::shared_ptr<openage::event::Event> & /* event */,
                                    const std::shared_ptr<openage::event::State> & /* state */) {
	// TODO
}

void DragSelectHandler::invoke(openage::event::EventLoop & /* loop */,
                               const std::shared_ptr<openage::event::EventEntity> & /* target */,
                               const std::shared_ptr<openage::event::State> &state,
                               const time::time_t &time,
                               const param_map &params) {
	auto gstate = std::dynamic_pointer_cast<openage::gamestate::GameState>(state);
	auto combat = gstate->get_combat();

	// XR fork: the controller sends a size_t (get("controlled", 0) read an int and always got 0)
	size_t controlled_id = params.get<size_t>("controlled", 0);

	Eigen::Matrix4f id_matrix = Eigen::Matrix4f::Identity();
	Eigen::Matrix4f cam_matrix = params.get("camera_matrix", id_matrix);
	Eigen::Vector2f drag_start = params.get("drag_start", Eigen::Vector2f{0, 0});
	Eigen::Vector2f drag_end = params.get("drag_end", Eigen::Vector2f{0, 0});
	// XR fork: single click, Shift, double click (see input/controller/game/controller.cpp)
	const bool click = params.get("click", false);
	const bool additive = params.get("additive", false);
	const bool same_type = params.get("same_type", false);
	using selection_cb_t = std::function<std::vector<entity_id_t>()>;
	const auto current_cb = params.get("current_cb", selection_cb_t{});
	const auto current = current_cb ? current_cb() : params.get("current", std::vector<entity_id_t>{});

	// Boundaries of the rectangle
	float top = std::max(drag_start.y(), drag_end.y());
	float bottom = std::min(drag_start.y(), drag_end.y());
	float left = std::min(drag_start.x(), drag_end.x());
	float right = std::max(drag_start.x(), drag_end.x());

	log::log(SPAM << "Drag select rectangle (NDC):");
	log::log(SPAM << "\tTop: " << top);
	log::log(SPAM << "\tBottom: " << bottom);
	log::log(SPAM << "\tLeft: " << left);
	log::log(SPAM << "\tRight: " << right);

	auto owner_of = [&](const std::shared_ptr<GameEntity> &entity) {
		auto owner = std::dynamic_pointer_cast<component::Ownership>(
			entity->get_component(component::component_t::OWNERSHIP));
		return owner ? std::optional<size_t>{owner->get_owners().get(time)} : std::nullopt;
	};
	// selectable and alive (dead entities play their death animation)
	auto selectable = [&](const std::shared_ptr<GameEntity> &entity) {
		return entity->has_component(component::component_t::SELECTABLE)
		       and not combat->is_dead(entity->get_id());
	};
	auto own = [&](const std::shared_ptr<GameEntity> &entity) {
		return owner_of(entity) == controlled_id;
	};
	auto clip_of = [&](const std::shared_ptr<GameEntity> &entity) {
		auto pos = std::dynamic_pointer_cast<component::Position>(
			entity->get_component(component::component_t::POSITION));
		auto world_pos = pos->get_positions().get(time).to_scene3().to_world_space();
		return Eigen::Vector4f{cam_matrix * Eigen::Vector4f{world_pos.x(), world_pos.y(), world_pos.z(), 1}};
	};
	auto entity_of = [&](entity_id_t id) -> std::shared_ptr<GameEntity> {
		const auto &entities = gstate->get_game_entities();
		auto it = entities.find(id);
		return it == entities.end() ? nullptr : it->second;
	};

	std::vector<entity_id_t> hits;
	bool hits_foreign = false;
	std::string how;
	if (click) {
		// XR fork: entity under the cursor from the object ids of the world pass;
		// without one (e.g. between the pixels of a thin sprite) the own unit
		// closest to the cursor in screen space
		std::shared_ptr<GameEntity> picked;
		if (params.contains("picked")) {
			picked = entity_of(params.get<entity_id_t>("picked"));
			if (picked and not selectable(picked)) {
				picked = nullptr;
			}
			how = "id buffer";
		}
		if (not picked) {
			PickRequest request;
			request.camera = cam_matrix;
			request.ndc = drag_end;
			request.slack = 0.01;
			picked = pick_entity(gstate, time, request, [&](const std::shared_ptr<GameEntity> &entity) -> std::optional<PickShape> {
				if (not selectable(entity) or not own(entity)) {
					return std::nullopt;
				}
				auto stats = combat->get_stats_snapshot(entity->get_id());
				if (not stats) {
					return PickShape{};
				}
				return PickShape{stats->radius, stats->building ? stats->radius : 1.0};
			});
			if (picked) {
				how = "near the cursor";
			}
		}

		if (picked) {
			hits_foreign = not own(picked);
			if (same_type and not hits_foreign) {
				// double click: all own entities of the type on screen
				auto stats = combat->get_stats_snapshot(picked->get_id());
				for (auto &entity : gstate->get_game_entities()) {
					if (not selectable(entity.second) or not own(entity.second)) {
						continue;
					}
					auto other = combat->get_stats_snapshot(entity.first);
					bool same = entity.first == picked->get_id()
					            or (stats and other and other->name == stats->name);
					auto clip = clip_of(entity.second);
					if (same and select::on_screen(clip.x(), clip.y())) {
						hits.push_back(entity.first);
					}
				}
				how += ", same type on screen";
			}
			else {
				hits.push_back(picked->get_id());
			}
		}
	}
	else {
		for (auto &entity : gstate->get_game_entities()) {
			if (not selectable(entity.second)) {
				// skip entities that are not selectable
				continue;
			}

			// Check if the entity is owned by the controlled player
			// TODO: Check this using Selectable diplomatic property
			if (not own(entity.second)) {
				// only select entities of the controlled player
				continue;
			}

			// Get the position of the entity in the viewport
			auto clip_pos = clip_of(entity.second);

			// Check if the entity is in the rectangle
			if (clip_pos.x() > left
			    and clip_pos.x() < right
			    and clip_pos.y() > bottom
			    and clip_pos.y() < top) {
				hits.push_back(entity.first);
			}
		}
	}

	// XR fork: Shift adds/toggles; a foreign entity is selected alone (only displayed)
	bool current_foreign = false;
	for (auto id : current) {
		auto entity = entity_of(id);
		if (entity and not own(entity)) {
			current_foreign = true;
		}
	}
	auto selected = select::next_selection(current, current_foreign, hits, hits_foreign, additive, click);
	std::erase_if(selected, [&](entity_id_t id) {
		auto entity = entity_of(id);
		return not entity or combat->was_removed(id);
	});

	// Select the units
	auto select_cb = params.get("select_cb",
	                            std::function<void(const std::vector<entity_id_t> ids)>{
									[](const std::vector<entity_id_t> /* ids */) {}});
	select_cb(selected);
	// XR fork: INFO, embedders diagnose taps on headsets from the log
	if (click) {
		std::string what = "nothing";
		if (not hits.empty()) {
			auto stats = combat->get_stats_snapshot(hits.front());
			auto entity = entity_of(hits.front());
			auto owner = entity ? owner_of(entity) : std::nullopt;
			what = "entity " + std::to_string(hits.front()) + (stats ? " " + stats->name : std::string{})
			       + (owner ? " of player " + std::to_string(*owner) : std::string{})
			       + (hits_foreign ? " (foreign, display only)" : "")
			       + (hits.size() > 1 ? " + " + std::to_string(hits.size() - 1) + " more" : "");
		}
		log::log(INFO << "Click select (NDC " << drag_end.x() << ", " << drag_end.y() << ")"
		              << (additive ? " + Shift" : "") << ": " << what
		              << (how.empty() ? "" : " [" + how + "]") << " -> " << selected.size()
		              << " selected of player " << controlled_id);
	}
	else {
		log::log(INFO << "Drag select (NDC x " << left << ".." << right << ", y " << bottom << ".." << top
		              << ")" << (additive ? " + Shift" : "") << ": " << hits.size() << " entities of player "
		              << controlled_id << " -> " << selected.size() << " selected");
	}
}

time::time_t DragSelectHandler::predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> & /* target */,
                                                    const std::shared_ptr<openage::event::State> & /* state */,
                                                    const time::time_t &at) {
	return at;
}


} // namespace openage::gamestate::event
