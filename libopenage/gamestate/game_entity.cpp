// Copyright 2022-2024 the openage authors. See copying.md for legal info.

#include "game_entity.h"

#include "gamestate/api/ability.h"
#include "gamestate/api/animation.h"
#include "gamestate/api/property.h"
#include "gamestate/component/api/idle.h"
#include "gamestate/component/api/move.h"
#include "gamestate/component/base_component.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "renderer/stages/world/render_entity.h"

namespace openage::gamestate {

GameEntity::GameEntity(entity_id_t id) :
	id{id},
	components{},
	render_entity{nullptr} {
}

std::shared_ptr<GameEntity> GameEntity::copy(entity_id_t id) {
	auto copy = std::shared_ptr<GameEntity>(new GameEntity(*this));
	copy->set_id(id);

	return copy;
}

entity_id_t GameEntity::get_id() const {
	return this->id;
}

void GameEntity::set_render_entity(const std::shared_ptr<renderer::world::RenderEntity> &entity) {
	// TODO: Transfer state from old render entity to new one?

	this->render_entity = entity;

	// XR fork: show entities that were updated before the renderer was attached
	if (this->render_entity != nullptr and not this->last_animation_path.empty()) {
		this->render_update(this->last_render_time, this->last_animation_path);
	}
}

void GameEntity::set_manager(const std::shared_ptr<GameEntityManager> &manager) {
	this->manager = manager;
}

const std::shared_ptr<GameEntityManager> &GameEntity::get_manager() const {
	return this->manager;
}

const std::shared_ptr<component::Component> &GameEntity::get_component(component::component_t type) {
	return this->components.at(type);
}

void GameEntity::add_component(const std::shared_ptr<component::Component> &component) {
	this->components.insert({component->get_type(), component});
}

void GameEntity::remove_component(component::component_t type) {
	this->components.erase(type);
}

bool GameEntity::has_component(component::component_t type) {
	return this->components.contains(type);
}

void GameEntity::render_update(const time::time_t &time,
                               const std::string &animation_path) {
	this->last_animation_path = animation_path;
	this->last_render_time = time;
	if (this->render_entity != nullptr) {
		const auto &pos = dynamic_pointer_cast<component::Position>(
							  this->components.at(component::component_t::POSITION))
		                      ->get_positions();
		const auto &angle = dynamic_pointer_cast<component::Position>(
								this->components.at(component::component_t::POSITION))
		                        ->get_angles();
		// XR fork: player color
		auto owner = this->components.find(component::component_t::OWNERSHIP);
		if (owner != this->components.end()) {
			auto ownership = dynamic_pointer_cast<component::Ownership>(owner->second);
			this->render_entity->set_player(static_cast<uint32_t>(ownership->get_owners().get(time)));
		}
		this->render_entity->update(this->id, pos, angle, animation_path, time);
	}
}

void GameEntity::remove_render_entity() {
	if (this->render_entity != nullptr) {
		this->render_entity->mark_removed();
	}
	this->render_entity = nullptr;
	this->last_animation_path.clear();
}

void GameEntity::set_id(entity_id_t id) {
	this->id = id;
}

} // namespace openage::gamestate
