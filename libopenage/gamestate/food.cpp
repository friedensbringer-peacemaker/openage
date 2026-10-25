// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "food.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <set>

#include "event/event.h"
#include "event/event_loop.h"
#include "log/log.h"
#include "log/message.h"

#include "coord/phys.h"
#include "coord/tile.h"
#include "gamestate/api/ability.h"
#include "gamestate/api/animation.h"
#include "gamestate/api/property.h"
#include "gamestate/api/terrain.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/constructable.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/build.h"
#include "gamestate/component/internal/commands/move.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"
#include "gamestate/pick.h"
#include "gamestate/player.h"
#include "gamestate/production.h"
#include "gamestate/production_math.h"
#include "gamestate/terrain.h"
#include "gamestate/terrain_chunk.h"


namespace openage::gamestate::food {

namespace {

template <typename T>
std::shared_ptr<T> component_of(const std::shared_ptr<GameEntity> &entity, component::component_t type) {
	if (entity == nullptr or not entity->has_component(type)) {
		return nullptr;
	}
	return std::dynamic_pointer_cast<T>(entity->get_component(type));
}

std::shared_ptr<GameEntity> find_entity(const std::shared_ptr<GameState> &state, entity_id_t id) {
	const auto &entities = state->get_game_entities();
	auto it = entities.find(id);
	return it == entities.end() ? nullptr : it->second;
}

coord::phys3 position_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto pos = component_of<component::Position>(entity, component::component_t::POSITION);
	return pos->get_positions().get(time);
}

std::optional<player_id_t> owner_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto owner = component_of<component::Ownership>(entity, component::component_t::OWNERSHIP);
	if (owner == nullptr) {
		return std::nullopt;
	}
	return owner->get_owners().get(time);
}

/// nyan game entity of an entity, from the names of its abilities
std::string fqon_of(const std::shared_ptr<GameEntity> &entity) {
	for (auto type : {component::component_t::IDLE,
	                  component::component_t::SELECTABLE,
	                  component::component_t::LIVE,
	                  component::component_t::CONSTRUCTABLE,
	                  component::component_t::HARVESTABLE}) {
		if (not entity->has_component(type)) {
			continue;
		}
		auto api = std::dynamic_pointer_cast<component::APIComponent>(entity->get_component(type));
		if (api == nullptr) {
			continue;
		}
		const auto &ability = api->get_ability().get_name();
		auto dot = ability.rfind('.');
		if (dot != std::string::npos) {
			return ability.substr(0, dot);
		}
	}
	return {};
}

std::string short_of(const std::string &fqon) {
	auto dot = fqon.rfind('.');
	return dot == std::string::npos ? fqon : fqon.substr(dot + 1);
}

std::string object_name(const nyan::ValueHolder &value) {
	return std::dynamic_pointer_cast<nyan::ObjectValue>(value.get_ptr())->get_name();
}

std::string tile_str(const coord::phys3 &pos) {
	return "(" + std::to_string(static_cast<int>(std::floor(pos.ne.to_double()))) + ", "
	       + std::to_string(static_cast<int>(std::floor(pos.se.to_double()))) + ")";
}

/// first animation of an ability (sprite path as the systems use it), empty if none
std::string animation_of(const nyan::Object &ability) {
	if (api::APIAbility::check_property(ability, api::ability_property_t::ANIMATED)) {
		auto property = api::APIAbility::get_property(ability, api::ability_property_t::ANIMATED);
		auto animations = api::APIAbilityProperty::get_animations(property);
		auto paths = api::APIAnimation::get_animation_paths(animations);
		if (not paths.empty()) {
			return prod::sprite_path(paths[0]);
		}
	}
	return {};
}

/// deterministic value in [-1, 1] from an entity id and a time (flee direction)
double jitter(entity_id_t id, const time::time_t &time) {
	uint64_t z = static_cast<uint64_t>(id) * 0x9E3779B97F4A7C15ULL
	             ^ static_cast<uint64_t>(std::llround(time.to_double() * 1000.0));
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z ^= z >> 31;
	return static_cast<double>(z % 20001) / 10000.0 - 1.0;
}

/// chunk and tile index of a tile on the terrain
std::optional<std::pair<size_t, size_t>> tile_index(const std::shared_ptr<GameState> &state, long ne, long se) {
	auto map = state->get_map();
	if (map == nullptr) {
		return std::nullopt;
	}
	const auto size = map->get_size();
	if (ne < 0 or se < 0 or ne >= static_cast<long>(size[0]) or se >= static_cast<long>(size[1])) {
		return std::nullopt;
	}
	auto terrain = map->get_terrain();
	auto chunk_size = terrain->get_chunk(0)->get_size();
	const long side = static_cast<long>(std::max(chunk_size[0], chunk_size[1]));
	const long chunks_ne = static_cast<long>(terrain->get_chunks_size()[0]);
	auto chunk_idx = static_cast<size_t>((ne / side) + (se / side) * chunks_ne);
	auto idx = static_cast<size_t>((ne % side) + (se % side) * side);
	return std::pair{chunk_idx, idx};
}

} // namespace


FoodState::FoodState(const std::shared_ptr<openage::event::EventLoop> &loop) :
	EventEntity{loop},
	loop{loop},
	handler{std::make_shared<FoodEventHandler>()} {
}

size_t FoodState::id() const {
	// fixed id, the food economy exists once per game ("food")
	return 0x666f6f64;
}

std::string FoodState::idstr() const {
	return "food";
}

void FoodState::start(const std::shared_ptr<GameState> &state,
                      const std::unordered_set<player_id_t> &neutral,
                      const time::time_t &time) {
	if (this->started) {
		return;
	}
	this->started = true;
	this->neutral = neutral;
	this->last_tick = time.to_double();
	std::weak_ptr<GameState> weak_state = state;
	std::weak_ptr<FoodState> weak_self = this->shared_from_this();
	auto combat = state->get_combat();
	combat->on_death([weak_state, weak_self](const std::shared_ptr<GameEntity> &entity, const time::time_t &t) {
		auto s = weak_state.lock();
		auto self = weak_self.lock();
		return s and self and self->on_death(s, entity, t);
	});
	combat->on_hit([weak_state, weak_self](const std::shared_ptr<GameEntity> &attacker,
	                                       const std::shared_ptr<GameEntity> &target,
	                                       const time::time_t &t) {
		auto s = weak_state.lock();
		auto self = weak_self.lock();
		if (s and self) {
			self->on_hit(s, attacker, target, t);
		}
	});
	size_t herdables = 0;
	size_t animals = 0;
	for (const auto &[id, entity] : state->get_game_entities()) {
		auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
		if (h and h->kind == source_t::HERDABLE) {
			herdables += 1;
		}
		else if (h and is_animal(h->kind)) {
			animals += 1;
		}
	}
	log::log(INFO << "Food: started, " << herdables << " herdables, " << animals << " wild animals (tick " << TICK
	              << " s)");
	this->tick(state, time);
}

bool FoodState::is_started() const {
	return this->started;
}

void FoodState::tick(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	if (not this->started) {
		return;
	}
	const double now = time.to_double();
	const double dt = std::max(0.0, now - this->last_tick);
	this->last_tick = now;
	this->herd(state, time);
	this->decay_carcasses(state, dt, time);
	this->fields_tick(state, time);
	if (now >= this->last_log + 30.0) {
		this->last_log = now;
		const auto &c = this->stats;
		log::log(INFO << "Food: " << c.conversions << " herdable conversions, " << this->carcasses.size()
		              << " carcasses (" << c.carcasses << " killed, " << c.carcasses_removed << " gathered/rotted, "
		              << std::lround(c.decayed_food) << " food decayed), " << c.flights << " flights, "
		              << this->fields.size() << " fields (" << c.fields_completed << " finished, " << c.fields_emptied
		              << " emptied, " << c.reseeds_used << " reseeded, " << c.fields_decayed << " decayed, "
		              << c.fields_rebuilt << " rebuilt, " << c.fields_removed << " removed), t=" << now << " s");
	}
	this->loop->create_event(this->handler,
	                         this->shared_from_this(),
	                         state,
	                         time + time::time_t::from_double(TICK),
	                         openage::event::EventHandler::param_map::map_t{});
}

// ---------------------------------------------------------------- herdables

void FoodState::herd(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	auto combat = state->get_combat();
	struct Eye {
		player_id_t player;
		double ne;
		double se;
		double sight;
		entity_id_t id;
	};
	std::vector<Eye> eyes;
	std::vector<std::pair<std::shared_ptr<GameEntity>, coord::phys3>> herdables;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (combat->is_dead(id)) {
			continue;
		}
		auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
		if (h and h->kind == source_t::HERDABLE) {
			herdables.emplace_back(entity, position_of(entity, time));
			continue;
		}
		auto stats = combat->get_stats(id);
		if (stats == nullptr or stats->animal or not(stats->unit or stats->building)) {
			continue;
		}
		auto owner = owner_of(entity, time);
		if (not owner or this->neutral.contains(*owner)) {
			continue;
		}
		auto pos = position_of(entity, time);
		eyes.push_back({*owner, pos.ne.to_double(), pos.se.to_double(), stats->line_of_sight, id});
	}
	if (herdables.empty()) {
		return;
	}
	std::sort(herdables.begin(), herdables.end(), [](const auto &a, const auto &b) {
		return a.first->get_id() < b.first->get_id();
	});
	for (const auto &[entity, pos] : herdables) {
		auto owner = owner_of(entity, time);
		if (not owner) {
			continue;
		}
		std::vector<Watcher> watchers;
		const double ne = pos.ne.to_double();
		const double se = pos.se.to_double();
		for (const auto &eye : eyes) {
			const double d = std::hypot(eye.ne - ne, eye.se - se);
			if (d <= std::max(eye.sight, HERD_CONVERT_RANGE) + 1e-9) {
				watchers.push_back({eye.player, d, eye.sight, eye.id});
			}
		}
		auto next = herd_owner(*owner, this->neutral.contains(*owner), watchers);
		if (not next or *next == *owner) {
			continue;
		}
		auto ownership = component_of<component::Ownership>(entity, component::component_t::OWNERSHIP);
		ownership->set_owner(time, static_cast<player_id_t>(*next));
		// the player colour of the sprite follows the owner
		if (not entity->get_animation_path().empty()) {
			entity->render_update(time, entity->get_animation_path());
		}
		this->stats.conversions += 1;
		log::log(INFO << "Food: " << prod::entity_name(entity) << " " << entity->get_id() << " at tile " << tile_str(pos)
		              << " now belongs to player " << *next << " (was " << *owner << "), t=" << time.to_double());
	}
}

// ---------------------------------------------------------------- carcasses

bool FoodState::on_death(const std::shared_ptr<GameState> &state,
                         const std::shared_ptr<GameEntity> &entity,
                         const time::time_t &time) {
	auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
	if (h == nullptr or not is_animal(h->kind) or h->is_depleted() or h->get_amount() <= 0.0) {
		return false;
	}
	h->dead = true;
	Carcass carcass;
	carcass.died_at = time.to_double();
	if (auto stats = state->get_combat()->get_stats(entity->get_id())) {
		carcass.death_time = stats->death_time;
	}
	// the despawn animation of the data shows the animal lying on the ground
	try {
		const auto fqon = fqon_of(entity);
		auto owner = owner_of(entity, time);
		auto view = owner and state->has_player(*owner) ? state->get_player(*owner)->get_db_view() : state->get_db_view();
		auto obj = view->get_object(fqon);
		for (const auto &ability_val : obj.get_set("GameEntity.abilities")) {
			auto ability = view->get_object(object_name(ability_val));
			if (ability.get_parents()[0] == "engine.ability.type.Despawn") {
				carcass.despawn_sprite = animation_of(ability);
				break;
			}
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Food: no despawn animation for entity " << entity->get_id() << ": " << err.what());
	}
	this->carcasses[entity->get_id()] = carcass;
	this->stats.carcasses += 1;
	log::log(INFO << "Food: " << prod::entity_name(entity) << " " << entity->get_id() << " killed at tile "
	              << tile_str(position_of(entity, time)) << ", carcass with " << h->get_amount() << " food (decay "
	              << h->decay_rate << "/s), t=" << time.to_double());
	return true;
}

void FoodState::on_hit(const std::shared_ptr<GameState> &state,
                       const std::shared_ptr<GameEntity> &attacker,
                       const std::shared_ptr<GameEntity> &target,
                       const time::time_t &time) {
	auto h = component_of<component::Harvestable>(target, component::component_t::HARVESTABLE);
	if (h == nullptr or h->kind != source_t::PREY or h->dead) {
		return;
	}
	auto combat = state->get_combat();
	auto stats = combat->get_stats(target->get_id());
	auto live = component_of<component::Live>(target, component::component_t::LIVE);
	if (stats == nullptr or live == nullptr) {
		return;
	}
	if (auto health = live->get_attribute(time, stats->health_attribute); health and *health <= 0) {
		return;
	}
	auto queue = component_of<component::CommandQueue>(target, component::component_t::COMMANDQUEUE);
	if (queue == nullptr) {
		return;
	}
	const auto pos = position_of(target, time);
	const auto from = position_of(attacker, time);
	const auto size = state->get_map()->get_size();
	auto [ne, se] = flee_point(pos.ne.to_double(), pos.se.to_double(), from.ne.to_double(), from.se.to_double(),
	                           FLEE_DISTANCE, jitter(target->get_id(), time), static_cast<double>(size[0]),
	                           static_cast<double>(size[1]));
	auto dest = state->get_map()->on_terrain(coord::phys3{coord::phys_t{ne}, coord::phys_t{se}, coord::phys_t{0.0}});
	queue->add_command(time, std::make_shared<component::command::MoveCommand>(dest));
	this->stats.flights += 1;
	if (this->stats.flights <= 20 or this->stats.flights % 25 == 0) {
		log::log(INFO << "Food: " << prod::entity_name(target) << " " << target->get_id() << " flees from tile "
		              << tile_str(pos) << " to " << tile_str(dest) << " (hit by " << prod::entity_name(attacker) << " "
		              << attacker->get_id() << "), t=" << time.to_double());
	}
}

void FoodState::decay_carcasses(const std::shared_ptr<GameState> &state, double dt, const time::time_t &time) {
	const double now = time.to_double();
	std::vector<entity_id_t> empty;
	for (auto &[id, carcass] : this->carcasses) {
		auto entity = find_entity(state, id);
		auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
		if (h == nullptr or h->is_depleted()) {
			empty.push_back(id);
			continue;
		}
		if (not carcass.switched and now >= carcass.died_at + carcass.death_time) {
			carcass.switched = true;
			if (not carcass.despawn_sprite.empty()) {
				entity->render_update(time, carcass.despawn_sprite);
			}
		}
		const double before = h->get_amount();
		const double after = decay(before, h->decay_rate, dt);
		h->set_amount(after);
		this->stats.decayed_food += before - after;
		if (after <= 0.0) {
			empty.push_back(id);
		}
	}
	for (auto id : empty) {
		auto entity = find_entity(state, id);
		if (entity != nullptr) {
			log::log(INFO << "Food: carcass " << prod::entity_name(entity) << " " << id << " rotted away, t=" << now);
			this->carcass_empty(state, entity, time);
		}
		else {
			this->carcasses.erase(id);
		}
	}
}

void FoodState::carcass_empty(const std::shared_ptr<GameState> &state,
                              const std::shared_ptr<GameEntity> &entity,
                              const time::time_t &time) {
	if (entity == nullptr) {
		return;
	}
	if (auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE)) {
		h->set_depleted();
	}
	this->carcasses.erase(entity->get_id());
	this->stats.carcasses_removed += 1;
	if (state->get_game_entities().contains(entity->get_id())) {
		state->get_combat()->remove(state, entity->get_id(), time);
	}
}

// ---------------------------------------------------------------- fields

FoodState::Field *FoodState::field_of(entity_id_t id) {
	auto it = this->fields.find(id);
	return it == this->fields.end() ? nullptr : &it->second;
}

const FoodState::FieldTerrain &FoodState::terrain_of(const std::shared_ptr<GameState> &state, const nyan::fqon_t &fqon) {
	auto it = this->field_terrain.find(fqon);
	if (it != this->field_terrain.end()) {
		return it->second;
	}
	FieldTerrain t;
	auto view = state->get_db_view();
	try {
		auto obj = view->get_object(fqon);
		std::vector<std::pair<double, nyan::fqon_t>> stages;
		for (const auto &ability_val : obj.get_set("GameEntity.abilities")) {
			auto ability = view->get_object(object_name(ability_val));
			const auto &parent = ability.get_parents()[0];
			if (parent == "engine.ability.type.OverlayTerrain") {
				t.crops = ability.get_object("OverlayTerrain.terrain_overlay").get_name();
			}
			else if (parent == "engine.ability.type.Constructable") {
				for (const auto &progress_val : ability.get_set("Constructable.construction_progress")) {
					auto progress = view->get_object(object_name(progress_val));
					for (const auto &[key, value] : progress.get_dict("Progress.properties")) {
						if (object_name(key) != "engine.util.progress.property.type.TerrainOverlay") {
							continue;
						}
						auto overlay = view->get_object(object_name(value));
						stages.emplace_back(progress.get_float("Progress.right_boundary"),
						                    overlay.get_object("TerrainOverlay.terrain_overlay").get_name());
					}
				}
			}
		}
		std::sort(stages.begin(), stages.end());
		for (const auto &stage : stages) {
			// the first stage (0 %) repeats the second one in the data
			if (t.construction.empty() or t.construction.back() != stage.second) {
				t.construction.push_back(stage.second);
			}
		}
		// the harvested ground next to the crops terrain ("…farm_crops.FarmCrops" -> "…farm_harvested.FarmHarvested")
		if (not t.crops.empty()) {
			auto pos = t.crops.find(".terrain.");
			if (pos != std::string::npos) {
				const auto candidate = t.crops.substr(0, pos) + ".terrain.farm_harvested.farm_harvested.FarmHarvested";
				try {
					view->get_object(candidate);
					t.harvested = candidate;
				}
				catch (std::exception &) {
				}
			}
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Food: terrain of " << fqon << " not readable: " << err.what());
	}
	log::log(INFO << "Food: " << short_of(fqon) << " terrain: " << t.construction.size() << " construction stages, crops "
	              << (t.crops.empty() ? std::string{"-"} : short_of(t.crops)) << ", harvested "
	              << (t.harvested.empty() ? std::string{"-"} : short_of(t.harvested)));
	return this->field_terrain.emplace(fqon, std::move(t)).first->second;
}

void FoodState::show_terrain(const std::shared_ptr<GameState> &state, Field &field, int stage, const time::time_t &time) {
	// fishing traps stand on water: the ground does not change
	if (field.shown == stage or field.kind == source_t::TRAP) {
		return;
	}
	auto entity = find_entity(state, field.id);
	if (entity == nullptr) {
		return;
	}
	const auto &terrain = this->terrain_of(state, fqon_of(entity));
	nyan::fqon_t fqon;
	if (stage >= 0 and static_cast<size_t>(stage) < terrain.construction.size()) {
		fqon = terrain.construction[static_cast<size_t>(stage)];
	}
	else if (stage == -1) {
		fqon = terrain.crops;
	}
	else if (stage == -2) {
		fqon = terrain.harvested.empty() ? terrain.crops : terrain.harvested;
	}
	field.shown = stage;
	if (fqon.empty()) {
		return;
	}
	TerrainTile tile;
	try {
		tile.terrain = state->get_db_view()->get_object(fqon);
		tile.terrain_asset_path = api::APITerrain::get_terrain_path(tile.terrain);
	}
	catch (std::exception &err) {
		log::log(WARN << "Food: terrain " << fqon << " not usable: " << err.what());
		return;
	}
	std::set<size_t> chunks;
	auto map_terrain = state->get_map()->get_terrain();
	for (const auto &[ne, se] : field.tiles) {
		auto idx = tile_index(state, ne, se);
		if (not idx) {
			continue;
		}
		map_terrain->get_chunk(idx->first)->set_tile(idx->second, tile);
		chunks.insert(idx->first);
	}
	for (auto c : chunks) {
		map_terrain->get_chunk(c)->render_update(time);
	}
}

void FoodState::restore_terrain(const std::shared_ptr<GameState> &state, Field &field, const time::time_t &time) {
	std::set<size_t> chunks;
	auto map_terrain = state->get_map()->get_terrain();
	for (size_t i = 0; i < field.tiles.size() and i < field.original.size(); ++i) {
		auto idx = tile_index(state, field.tiles[i].first, field.tiles[i].second);
		if (not idx) {
			continue;
		}
		map_terrain->get_chunk(idx->first)->set_tile(idx->second, field.original[i]);
		chunks.insert(idx->first);
	}
	for (auto c : chunks) {
		map_terrain->get_chunk(c)->render_update(time);
	}
	field.shown = -100;
}

void FoodState::field_placed(const std::shared_ptr<GameState> &state,
                             const std::shared_ptr<GameEntity> &field,
                             const time::time_t &time) {
	auto h = component_of<component::Harvestable>(field, component::component_t::HARVESTABLE);
	auto constructable = component_of<component::Constructable>(field, component::component_t::CONSTRUCTABLE);
	if (h == nullptr or not is_field(h->kind) or this->fields.contains(field->get_id())) {
		return;
	}
	Field f;
	f.id = field->get_id();
	f.owner = owner_of(field, time).value_or(0);
	f.kind = h->kind;
	const auto pos = position_of(field, time);
	const double radius = constructable ? constructable->radius : h->radius;
	for (const auto &t : prod::building_tiles(pos.ne.to_double(), pos.se.to_double(), radius)) {
		f.tiles.emplace_back(t.ne, t.se);
		auto idx = tile_index(state, t.ne, t.se);
		if (idx) {
			f.original.push_back(state->get_map()->get_terrain()->get_chunk(idx->first)->get_tile(idx->second));
		}
		else {
			f.original.push_back(TerrainTile{});
		}
	}
	const bool complete = constructable == nullptr or constructable->is_complete();
	if (not complete) {
		f.state = field_t::BUILDING;
	}
	else if (h->get_amount() > 0.0) {
		f.state = field_t::GROWING;
	}
	else {
		f.state = field_t::DECAYED;
		f.decayed_at = time.to_double();
	}
	auto &stored = this->fields.emplace(f.id, std::move(f)).first->second;
	if (stored.state == field_t::BUILDING) {
		const auto &terrain = this->terrain_of(state, fqon_of(field));
		this->show_terrain(state, stored,
		                   static_cast<int>(construction_stage(constructable->get_progress(), terrain.construction.size())),
		                   time);
	}
	else {
		this->show_terrain(state, stored, stored.state == field_t::GROWING ? -1 : -2, time);
	}
}

void FoodState::field_progress(const std::shared_ptr<GameState> &state,
                               const std::shared_ptr<GameEntity> &field,
                               const time::time_t &time) {
	auto f = this->field_of(field->get_id());
	auto constructable = component_of<component::Constructable>(field, component::component_t::CONSTRUCTABLE);
	if (f == nullptr or constructable == nullptr or f->state != field_t::BUILDING) {
		return;
	}
	const auto &terrain = this->terrain_of(state, fqon_of(field));
	this->show_terrain(state, *f,
	                   static_cast<int>(construction_stage(constructable->get_progress(), terrain.construction.size())),
	                   time);
}

void FoodState::field_complete(const std::shared_ptr<GameState> &state,
                               const std::shared_ptr<GameEntity> &field,
                               const time::time_t &time) {
	auto h = component_of<component::Harvestable>(field, component::component_t::HARVESTABLE);
	if (h == nullptr or not is_field(h->kind)) {
		return;
	}
	if (not this->fields.contains(field->get_id())) {
		this->field_placed(state, field, time);
	}
	auto f = this->field_of(field->get_id());
	// food of the data in the view of the owner (mill technologies patch the farm)
	double amount = h->get_start_amount();
	try {
		auto owner = owner_of(field, time);
		auto view = owner and state->has_player(*owner) ? state->get_player(*owner)->get_db_view() : state->get_db_view();
		auto obj = view->get_object(fqon_of(field));
		for (const auto &ability_val : obj.get_set("GameEntity.abilities")) {
			auto ability = view->get_object(object_name(ability_val));
			if (ability.get_parents()[0] == "engine.ability.type.Harvestable") {
				amount = static_cast<double>(
					ability.get_object("Harvestable.resources").get_int("ResourceSpot.starting_amount"));
				break;
			}
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Food: food of field " << field->get_id() << " not readable: " << err.what());
	}
	h->refill(amount);
	h->worker.reset();
	f->state = field_t::GROWING;
	this->show_terrain(state, *f, -1, time);
	this->stats.fields_completed += 1;
	log::log(INFO << "Food: " << prod::entity_name(field) << " " << field->get_id() << " of player " << f->owner
	              << " finished with " << amount << " food, t=" << time.to_double());
}

std::optional<std::pair<double, resource_amounts_t>> FoodState::field_build_data(const std::shared_ptr<GameEntity> &builder,
                                                                                const std::string &name) const {
	auto comp = component_of<component::Builder>(builder, component::component_t::BUILDER);
	if (comp == nullptr) {
		return std::nullopt;
	}
	const auto *b = comp->find(name);
	if (b == nullptr) {
		return std::nullopt;
	}
	return std::pair{b->time, b->cost};
}

void FoodState::start_foundation(const std::shared_ptr<GameState> &state,
                                 const std::shared_ptr<GameEntity> &field,
                                 double build_time,
                                 const time::time_t &time) {
	auto constructable = component_of<component::Constructable>(field, component::component_t::CONSTRUCTABLE);
	auto h = component_of<component::Harvestable>(field, component::component_t::HARVESTABLE);
	auto f = this->field_of(field->get_id());
	if (constructable == nullptr or h == nullptr or f == nullptr) {
		return;
	}
	constructable->start_foundation(build_time);
	prod::update_building_health(state, field, -1.0, time);
	h->set_amount(0.0);
	h->worker.reset();
	f->state = field_t::BUILDING;
	this->show_terrain(state, *f, 0, time);
}

empty_t FoodState::field_empty(const std::shared_ptr<GameState> &state,
                               const std::shared_ptr<GameEntity> &field,
                               const std::shared_ptr<GameEntity> &worker,
                               const time::time_t &time) {
	auto h = component_of<component::Harvestable>(field, component::component_t::HARVESTABLE);
	if (h == nullptr or not is_field(h->kind)) {
		return empty_t::DECAY;
	}
	if (not this->fields.contains(field->get_id())) {
		this->field_placed(state, field, time);
	}
	auto f = this->field_of(field->get_id());
	this->stats.fields_emptied += 1;
	auto &queue = this->queues[f->owner];
	const bool farmer = worker != nullptr and not state->get_combat()->is_dead(worker->get_id());
	auto decision = on_field_empty(h->kind, queue.size(), farmer);
	std::optional<std::pair<double, resource_amounts_t>> data;
	if (decision == empty_t::RESEED) {
		data = this->field_build_data(worker, prod::entity_name(field));
		if (not data) {
			decision = empty_t::DECAY;
		}
	}
	if (decision == empty_t::RESEED) {
		queue.take();
		this->start_foundation(state, field, data->first, time);
		auto commands = component_of<component::CommandQueue>(worker, component::component_t::COMMANDQUEUE);
		if (commands) {
			commands->add_command(time, std::make_shared<component::command::BuildCommand>(field->get_id()));
		}
		this->stats.reseeds_used += 1;
		log::log(INFO << "Food: " << prod::entity_name(field) << " " << field->get_id() << " of player " << f->owner
		              << " reseeded from the mill queue (" << queue.size() << " left), unit " << worker->get_id()
		              << " builds it, t=" << time.to_double());
		return empty_t::RESEED;
	}
	h->set_amount(0.0);
	h->worker.reset();
	f->state = field_t::DECAYED;
	f->decayed_at = time.to_double();
	this->show_terrain(state, *f, -2, time);
	this->stats.fields_decayed += 1;
	log::log(INFO << "Food: " << prod::entity_name(field) << " " << field->get_id() << " of player " << f->owner
	              << " is empty and decays (no reseed queued), t=" << time.to_double());
	return empty_t::DECAY;
}

std::string FoodState::rebuild(const std::shared_ptr<GameState> &state,
                               const std::shared_ptr<GameEntity> &field,
                               const std::shared_ptr<GameEntity> &builder,
                               const time::time_t &time) {
	auto f = field ? this->field_of(field->get_id()) : nullptr;
	if (f == nullptr or f->state != field_t::DECAYED) {
		return "Kein verdorrtes Feld";
	}
	auto owner = owner_of(builder, time);
	if (not owner or *owner != f->owner) {
		return "Fremdes Feld";
	}
	const auto name = prod::entity_name(field);
	auto data = this->field_build_data(builder, name);
	if (not data) {
		return "Die Auswahl kann " + prod::label_of(name) + " nicht bauen";
	}
	auto &stock = state->get_player(*owner)->get_resources();
	if (not stock.spend(data->second)) {
		auto missing = prod::missing_resource(stock.get(), data->second);
		return missing ? prod::missing_message(*missing) : std::string{"Nicht genug Rohstoffe"};
	}
	this->start_foundation(state, field, data->first, time);
	auto commands = component_of<component::CommandQueue>(builder, component::component_t::COMMANDQUEUE);
	if (commands) {
		commands->add_command(time, std::make_shared<component::command::BuildCommand>(field->get_id()));
	}
	this->stats.fields_rebuilt += 1;
	log::log(INFO << "Food: " << name << " " << field->get_id() << " of player " << *owner << " built again by unit "
	              << builder->get_id() << ", t=" << time.to_double());
	return {};
}

bool FoodState::is_decayed(entity_id_t field) const {
	auto it = this->fields.find(field);
	return it != this->fields.end() and it->second.state == field_t::DECAYED;
}

bool FoodState::field_tile(long ne, long se) const {
	for (const auto &[id, f] : this->fields) {
		for (const auto &t : f.tiles) {
			if (t.first == ne and t.second == se) {
				return true;
			}
		}
	}
	return false;
}

std::optional<entity_id_t> FoodState::pick_decayed(const std::shared_ptr<GameState> &state,
                                                   const coord::phys3 &ground_hit,
                                                   player_id_t player,
                                                   const time::time_t &time) const {
	auto picked = pick_entity_ray(state, time, ground_hit, [&](const std::shared_ptr<GameEntity> &entity) -> std::optional<PickShape> {
		auto it = this->fields.find(entity->get_id());
		if (it == this->fields.end() or it->second.state != field_t::DECAYED or it->second.owner != player) {
			return std::nullopt;
		}
		auto constructable = component_of<component::Constructable>(entity, component::component_t::CONSTRUCTABLE);
		return PickShape{constructable ? constructable->radius : 1.5, 0.3};
	});
	if (not picked) {
		return std::nullopt;
	}
	return picked->get_id();
}

bool FoodState::claim(const std::shared_ptr<GameEntity> &field, entity_id_t worker) {
	auto h = component_of<component::Harvestable>(field, component::component_t::HARVESTABLE);
	if (h == nullptr or not is_field(h->kind)) {
		return true;
	}
	if (h->worker and *h->worker != worker) {
		return false;
	}
	h->worker = worker;
	return true;
}

void FoodState::release(const std::shared_ptr<GameEntity> &field, entity_id_t worker) {
	auto h = component_of<component::Harvestable>(field, component::component_t::HARVESTABLE);
	if (h != nullptr and h->worker and *h->worker == worker) {
		h->worker.reset();
	}
}

void FoodState::adopt_fields(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	std::vector<std::shared_ptr<GameEntity>> found;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (this->fields.contains(id)) {
			continue;
		}
		auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
		if (h != nullptr and is_field(h->kind) and not state->get_combat()->is_dead(id)) {
			found.push_back(entity);
		}
	}
	for (const auto &entity : found) {
		this->field_placed(state, entity, time);
	}
}

void FoodState::remove_field(const std::shared_ptr<GameState> &state, entity_id_t id, const time::time_t &time) {
	auto f = this->field_of(id);
	if (f == nullptr) {
		return;
	}
	this->restore_terrain(state, *f, time);
	auto entity = find_entity(state, id);
	if (entity != nullptr) {
		if (auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE)) {
			h->set_depleted();
		}
		state->get_combat()->remove(state, id, time);
	}
	this->fields.erase(id);
	this->stats.fields_removed += 1;
}

void FoodState::fields_tick(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	this->adopt_fields(state, time);
	const double now = time.to_double();
	auto combat = state->get_combat();
	std::vector<entity_id_t> expired;
	std::vector<entity_id_t> gone;
	for (auto &[id, f] : this->fields) {
		auto entity = find_entity(state, id);
		if (entity == nullptr or combat->is_dead(id)) {
			gone.push_back(id);
			continue;
		}
		auto h = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
		// the worker still works here (alive, its job names the field)
		if (h != nullptr and h->worker) {
			auto worker = find_entity(state, *h->worker);
			auto gather = component_of<component::Gather>(worker, component::component_t::GATHER);
			if (worker == nullptr or combat->is_dead(*h->worker) or gather == nullptr
			    or gather->get_job().phase == component::Gather::phase_t::NONE or gather->get_job().target != id) {
				h->worker.reset();
			}
		}
		if (f.state == field_t::DECAYED and field_expired(f.decayed_at, now)) {
			expired.push_back(id);
		}
	}
	for (auto id : gone) {
		auto f = this->field_of(id);
		if (f != nullptr) {
			// destroyed (or dying): the ground shows its old terrain again
			this->restore_terrain(state, *f, time);
			this->fields.erase(id);
		}
	}
	for (auto id : expired) {
		log::log(INFO << "Food: decayed field " << id << " removed after " << FIELD_DECAY_SECONDS << " s, t=" << now);
		this->remove_field(state, id, time);
	}
}

// ---------------------------------------------------------------- reseed queue

std::string FoodState::queue_reseed(const std::shared_ptr<GameState> &state, player_id_t player,
                                    const resource_amounts_t &cost) {
	auto &queue = this->queues[player];
	if (queue.size() >= ReseedQueue::MAX) {
		return "Warteschlange voll";
	}
	auto &stock = state->get_player(player)->get_resources();
	if (not stock.spend(cost)) {
		auto missing = prod::missing_resource(stock.get(), cost);
		return missing ? prod::missing_message(*missing) : std::string{"Nicht genug Rohstoffe"};
	}
	ReseedQueue::cost_t c{};
	for (size_t i = 0; i < c.size() and i < RESOURCE_COUNT; ++i) {
		c[i] = cost[i];
	}
	queue.push(c);
	log::log(INFO << "Food: player " << player << " queues a farm reseed (" << queue.size() << " queued)");
	return {};
}

bool FoodState::cancel_reseed(const std::shared_ptr<GameState> &state, player_id_t player, size_t index) {
	auto &queue = this->queues[player];
	auto cost = queue.cancel_at(index);
	if (not cost) {
		return false;
	}
	auto &stock = state->get_player(player)->get_resources();
	for (size_t i = 0; i < cost->size() and i < RESOURCE_COUNT; ++i) {
		stock.add(static_cast<resource_t>(i), (*cost)[i]);
	}
	log::log(INFO << "Food: player " << player << " cancels reseed " << index << ", refunded (" << queue.size()
	              << " queued)");
	return true;
}

size_t FoodState::reseeds(player_id_t player) const {
	auto it = this->queues.find(player);
	return it == this->queues.end() ? 0 : it->second.size();
}

std::vector<resource_amounts_t> FoodState::reseed_costs(player_id_t player) const {
	std::vector<resource_amounts_t> out;
	auto it = this->queues.find(player);
	if (it == this->queues.end()) {
		return out;
	}
	for (const auto &c : it->second.get()) {
		resource_amounts_t r{};
		for (size_t i = 0; i < c.size() and i < RESOURCE_COUNT; ++i) {
			r[i] = c[i];
		}
		out.push_back(r);
	}
	return out;
}

void FoodState::set_reseeds(player_id_t player, const std::vector<resource_amounts_t> &costs) {
	auto &queue = this->queues[player];
	queue.clear();
	for (const auto &r : costs) {
		ReseedQueue::cost_t c{};
		for (size_t i = 0; i < c.size() and i < RESOURCE_COUNT; ++i) {
			c[i] = r[i];
		}
		queue.push(c);
	}
}

FoodState::Counters FoodState::counters() const {
	return this->stats;
}

// ---------------------------------------------------------------- event handler

FoodEventHandler::FoodEventHandler() :
	OnceEventHandler{"game.food_tick"} {
}

void FoodEventHandler::setup_event(const std::shared_ptr<openage::event::Event> & /* event */,
                                   const std::shared_ptr<openage::event::State> & /* state */) {
}

void FoodEventHandler::invoke(openage::event::EventLoop & /* loop */,
                              const std::shared_ptr<openage::event::EventEntity> &target,
                              const std::shared_ptr<openage::event::State> &state,
                              const time::time_t &time,
                              const param_map & /* params */) {
	auto food = std::dynamic_pointer_cast<FoodState>(target);
	auto gstate = std::dynamic_pointer_cast<GameState>(state);
	if (food and gstate) {
		food->tick(gstate, time);
	}
}

time::time_t FoodEventHandler::predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> & /* target */,
                                                   const std::shared_ptr<openage::event::State> & /* state */,
                                                   const time::time_t &at) {
	return at;
}

} // namespace openage::gamestate::food
