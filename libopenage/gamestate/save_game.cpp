// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "save_game.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <unordered_map>
#include <unordered_set>

#include "log/log.h"
#include "log/message.h"

#include "coord/phys.h"
#include "coord/tile.h"
#include "gamestate/building_place.h"
#include "gamestate/buildings.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/stats.h"
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/constructable.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/api/live.h"
#include "gamestate/component/api/production_queue.h"
#include "gamestate/component/api_component.h"
#include "gamestate/component/internal/activity.h"
#include "gamestate/component/internal/command_queue.h"
#include "gamestate/component/internal/commands/build.h"
#include "gamestate/component/internal/commands/gather.h"
#include "gamestate/component/internal/commands/move.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/econ.h"
#include "gamestate/econ_math.h"
#include "gamestate/entity_factory.h"
#include "gamestate/food.h"
#include "gamestate/food_math.h"
#include "gamestate/special.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/manager.h"
#include "gamestate/map.h"
#include "gamestate/player.h"
#include "gamestate/production.h"
#include "gamestate/production_math.h"
#include "gamestate/tech.h"
#include "gamestate/vision/vision_state.h"
#include "version.h"


namespace openage::gamestate::save {

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

/// nyan game entity of an entity ("...villager.Villager"), from the names of its abilities
std::string fqon_of(const std::shared_ptr<GameEntity> &entity) {
	for (auto type : {component::component_t::IDLE,
	                  component::component_t::SELECTABLE,
	                  component::component_t::MOVE,
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

bool is_generated(std::pair<entity_id_t, entity_id_t> generated, entity_id_t id) {
	return generated.second >= generated.first and id >= generated.first and id <= generated.second;
}

/// footprint tiles of a building at its anchor
std::vector<coord::tile> building_footprint(const coord::phys3 &pos, double radius) {
	std::vector<coord::tile> tiles;
	for (const auto &t : prod::building_tiles(pos.ne.to_double(), pos.se.to_double(), radius)) {
		tiles.push_back(coord::tile{t.ne, t.se});
	}
	return tiles;
}

/// remove an entity at once (no death animation): tiles, renderer, state, combat
void remove_now(const std::shared_ptr<GameState> &state,
                const std::shared_ptr<GameEntity> &entity,
                const time::time_t &time) {
	auto harvestable = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
	if (harvestable != nullptr and not harvestable->is_depleted()) {
		harvestable->set_depleted();
		auto position = component_of<component::Position>(entity, component::component_t::POSITION);
		auto pos = position->get_positions().get(time);
		for (const auto &t : econ::footprint(pos.ne.to_double(), pos.se.to_double(), harvestable->radius)) {
			state->get_map()->unblock_tile(coord::tile{t.ne, t.se}, time);
		}
	}
	auto combat = state->get_combat();
	if (combat != nullptr and combat->get_stats(entity->get_id()) != nullptr) {
		// buildings: frees the footprint; removes render entity and state entry
		combat->remove(state, entity->get_id(), time);
		return;
	}
	entity->remove_render_entity();
	state->remove_game_entity(entity->get_id());
}

void set_health(const std::shared_ptr<GameState> &state,
                const std::shared_ptr<GameEntity> &entity,
                int64_t health,
                const time::time_t &time) {
	auto combat = state->get_combat();
	auto stats = combat ? combat->get_stats(entity->get_id()) : nullptr;
	auto live = component_of<component::Live>(entity, component::component_t::LIVE);
	if (stats == nullptr or live == nullptr or stats->health_attribute.empty()) {
		return;
	}
	// as saved (resource spots have health 0; dying entities are never saved)
	const int64_t value = std::max<int64_t>(0, health);
	if (auto current = live->get_attribute(time, stats->health_attribute); current and *current == value) {
		return;
	}
	live->set_attribute(time, stats->health_attribute, value);
	combat->health_changed(entity->get_id(), value);
}

} // namespace


SaveData capture(const std::shared_ptr<GameState> &state,
                 const MapSettings &map,
                 std::pair<entity_id_t, entity_id_t> generated,
                 const time::time_t &time,
                 const std::string &title) {
	SaveData d;
	if (auto special_state = state->get_special()) {
		d.market = special_state->market_text();
	}
	d.title = title;
	d.created = now_text();
	d.engine = std::string{"openage "} + version::version + " xr";
	d.game_time = time.to_double();
	d.map = map;
	d.map.load_file.clear();
	d.generated_first = generated.first;
	d.generated_last = generated.second;
	const double now = time.to_double();
	auto combat = state->get_combat();

	// players
	std::vector<player_id_t> player_ids;
	for (player_id_t id = 0; id < 16; ++id) {
		if (state->has_player(id)) {
			player_ids.push_back(id);
		}
	}
	for (auto id : player_ids) {
		SavedPlayer p;
		p.id = id;
		p.resources = state->get_player(id)->get_resources().get();
		// XR fork W1: civilisation, researched technologies, completed building types
		if (const auto *t = state->get_tech() ? state->get_tech()->find(id) : nullptr) {
			p.civ = t->civ;
			p.techs = t->order;
			p.built.assign(t->facts.built.begin(), t->facts.built.end());
		}
		// vision (XR fork): explored tiles (the sight follows the entities after loading)
		if (auto vision = state->get_vision(); vision and vision->is_started() and vision->get_mode() != vision_mode_t::ALL) {
			p.explored = vision->explored_text(id);
		}
		// XR fork W3: farm reseeds queued at the mill (costs paid)
		if (auto food_state = state->get_food()) {
			p.reseeds = food_state->reseed_costs(id);
		}
		// XR fork W7: stances of the player
		if (auto combat_state = state->get_combat()) {
			for (const auto &[pair, stance] : combat_state->get_stances()) {
				if (pair.first == id) {
					p.stances.emplace_back(pair.second, diplomacy::stance_name(stance));
				}
			}
		}
		d.players.push_back(p);
	}
	// the map is generated again with the civilisations that were played (random ones resolved)
	if (not d.map.civs.empty()) {
		for (const auto &p : d.players) {
			if (not p.civ.empty()) {
				if (p.id >= d.map.civs.size()) {
					d.map.civs.resize(p.id + 1, "random");
				}
				d.map.civs[p.id] = p.civ;
			}
		}
	}

	// entities, sorted by id (deterministic file)
	std::vector<std::pair<entity_id_t, std::shared_ptr<GameEntity>>> entities(state->get_game_entities().begin(),
	                                                                            state->get_game_entities().end());
	std::sort(entities.begin(), entities.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	std::unordered_set<entity_id_t> present;
	for (const auto &[id, entity] : entities) {
		// XR fork W3: a carcass (dead animal with food) is saved, other dying entities as gone
		auto carcass = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
		const bool is_carcass = carcass != nullptr and carcass->dead and not carcass->is_depleted();
		if (combat != nullptr and combat->is_dead(id) and not is_carcass) {
			// dying: saved as gone
			continue;
		}
		if (not entity->has_component(component::component_t::POSITION)
		    or not entity->has_component(component::component_t::OWNERSHIP)) {
			continue;
		}
		const std::string fqon = fqon_of(entity);
		if (fqon.empty()) {
			log::log(WARN << "Save: entity " << id << " without a nyan type, not stored");
			continue;
		}
		present.insert(id);

		SavedEntity e;
		e.id = id;
		e.fqon = fqon;
		e.generated = is_generated(generated, id);
		auto position = component_of<component::Position>(entity, component::component_t::POSITION);
		auto owner = component_of<component::Ownership>(entity, component::component_t::OWNERSHIP);
		e.owner = owner->get_owners().get(time);
		auto pos = position->get_positions().get(time);
		e.ne = pos.ne.to_double();
		e.se = pos.se.to_double();
		e.up = pos.up.to_double();
		e.angle = position->get_angles().get(time).to_double();

		bool changed = not e.generated;
		// health
		if (auto live = component_of<component::Live>(entity, component::component_t::LIVE)) {
			auto stats = combat ? combat->get_stats(id) : nullptr;
			if (stats != nullptr and not stats->health_attribute.empty()) {
				if (auto health = live->get_attribute(time, stats->health_attribute)) {
					e.health = *health;
					if (*health < stats->max_health) {
						changed = true;
					}
				}
			}
		}
		// resource spot
		if (auto harvestable = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE)) {
			e.harvest = harvestable->get_amount();
			if (std::fabs(harvestable->get_amount() - harvestable->get_start_amount()) > 1e-9) {
				changed = true;
			}
			// XR fork W3: animals walk, change owner and die: always stored
			if (food::is_animal(harvestable->kind)) {
				changed = true;
				e.dead = harvestable->dead;
				if (e.dead) {
					// a carcass has no health (the entity is dead)
					e.health.reset();
				}
			}
		}
		// gatherer: load and job
		if (auto gather = component_of<component::Gather>(entity, component::component_t::GATHER)) {
			changed = true;
			e.carried = gather->get_carried();
			e.carried_type = static_cast<int>(gather->get_carried_type());
			const auto &job = gather->get_job();
			if (job.phase != component::Gather::phase_t::NONE and job.target) {
				e.order = "gather";
				e.order_target = *job.target;
			}
		}
		// builder job
		if (auto builder = component_of<component::Builder>(entity, component::component_t::BUILDER)) {
			changed = true;
			const auto &job = builder->get_job();
			if (job.phase != component::Builder::phase_t::NONE and job.target) {
				e.order = "build";
				e.order_target = *job.target;
			}
			// XR fork W5 (version 6): the rest of its wall drag
			e.wall_queue.assign(builder->wall_queue.begin(), builder->wall_queue.end());
		}
		// XR fork W5 (version 6): gates
		if (auto buildings = state->get_buildings()) {
			if (const auto *gate = buildings->gate(id)) {
				changed = true;
				e.gate_dir = static_cast<int>(gate->dir);
				e.gate_locked = gate->locked;
			}
		}
		// attack
		if (combat != nullptr) {
			if (auto target = combat->get_target(id)) {
				e.order = "attack";
				e.order_target = *target;
			}
		}
		// XR fork W4 (version 4): stance, formation, orders, garrison, town bell
		if (combat != nullptr) {
			if (auto saved = combat->save_entity(id)) {
				changed = true;
				if (saved->stance != combat::stance_t::AGGRESSIVE) {
					e.stance = combat::to_string(saved->stance);
				}
				if (saved->formation != combat::formation_t::NONE) {
					e.formation = combat::to_string(saved->formation);
				}
				if (saved->order != combat::order_t::NONE) {
					e.combat_order = combat::to_string(saved->order);
					e.combat_entity = saved->order_entity.value_or(0);
					for (const auto &p : saved->route) {
						e.combat_route.push_back({p.x, p.y});
					}
				}
				if (saved->inside) {
					e.garrison = *saved->inside;
					e.garrison_bell = saved->by_bell;
					e.bell_ne = saved->bell_from.x;
					e.bell_se = saved->bell_from.y;
					if (saved->resume_build) {
						e.resume = "build";
						e.resume_entity = *saved->resume_build;
					}
					else if (saved->resume_gather) {
						e.resume = "gather";
						e.resume_entity = *saved->resume_gather;
					}
					// inside a building: no job of its own until it comes out
					e.order.clear();
				}
				e.bell = saved->bell;
			}
		}
		// XR fork W6 (version 6): faith, conversion, relics, cargo, trebuchet
		if (auto special_state = state->get_special()) {
			if (special_state->stowed_in(id)) {
				// in a ship, ram, monastery or a monk's hands: the carrier stores it (no W4 garrison)
				e.garrison.reset();
				e.garrison_bell = false;
				e.order.clear();
				changed = true;
			}
			if (special_state->is_relic(id)) {
				changed = true;  // relics are carried away and dropped elsewhere
			}
			if (auto s = special_state->save_entity(id)) {
				changed = true;
				e.faith = s->faith;
				if (s->convert_target) {
					e.convert_target = *s->convert_target;
					e.convert_time = s->convert_time;
				}
				if (s->relic_carry) {
					e.relic_carry = *s->relic_carry;
				}
				e.relic_store.assign(s->relic_store.begin(), s->relic_store.end());
				e.cargo.assign(s->cargo.begin(), s->cargo.end());
				e.siege_mode = s->siege_mode;
				if (s->trade_home) {
					e.trade_home = *s->trade_home;
					e.trade_partner = s->trade_partner;
					e.trade_to_partner = s->trade_to_partner;
					e.trade_goods = s->trade_goods;
				}
			}
		}
		// walking somewhere (last keyframe of the position curve lies in the future)
		if (e.order.empty() and entity->has_component(component::component_t::MOVE)) {
			const auto &frames = position->get_positions().get_container();
			if (frames.size() > 0) {
				const auto &last = *std::prev(frames.end());
				if (last.time() > time) {
					e.order = "move";
					e.order_ne = last.val().ne.to_double();
					e.order_se = last.val().se.to_double();
				}
			}
		}
		// training queue, rally point
		if (auto production = component_of<component::ProductionQueue>(entity, component::component_t::PRODUCTION_QUEUE)) {
			changed = true;
			bool first = true;
			for (const auto &item : production->get_queue().get_items()) {
				SavedQueueItem q;
				q.name = item.name;
				q.fqon = item.fqon;
				q.cost = item.cost;
				q.time = item.time;
				q.remaining = first and item.started >= 0.0 ? std::max(0.0, item.started + item.time - now) : -1.0;
				q.research = item.research;
				first = false;
				e.queue.push_back(q);
			}
			if (production->rally_point) {
				e.rally = std::array<double, 2>{production->rally_point->ne.to_double(),
				                                production->rally_point->se.to_double()};
				if (production->rally_target) {
					using rally_t = component::ProductionQueue::rally_t;
					const auto kind = production->rally_target->kind;
					e.rally_kind = kind == rally_t::RESOURCE ? "resource" : kind == rally_t::ENTITY ? "entity"
					             : kind == rally_t::ENEMY    ? "enemy"
					                                         : "";
					e.rally_entity = production->rally_target->entity;
					e.rally_resource = static_cast<int>(production->rally_target->resource);
				}
			}
		}
		// foundation
		if (auto constructable = component_of<component::Constructable>(entity, component::component_t::CONSTRUCTABLE)) {
			if (not constructable->is_complete()) {
				changed = true;
				e.construction = constructable->get_progress();
				e.build_time = constructable->get_build_time();
			}
		}
		if (changed) {
			d.entities.push_back(e);
		}
	}
	// generated objects that are gone
	for (entity_id_t id = generated.first; generated.second >= generated.first and id <= generated.second; ++id) {
		if (not present.contains(id)) {
			d.removed.push_back(id);
		}
	}
	log::log(INFO << "Save: captured t=" << d.game_time << " s, " << d.players.size() << " players, "
	              << d.entities.size() << " entities (" << d.removed.size() << " generated objects gone), map "
	              << map_text(d.map));
	return d;
}


RestoreResult restore(const SaveData &data,
                      const std::shared_ptr<GameState> &state,
                      const std::shared_ptr<openage::event::EventLoop> &loop,
                      const std::shared_ptr<EntityFactory> &factory,
                      const std::shared_ptr<prod::Production> &production,
                      std::pair<entity_id_t, entity_id_t> generated,
                      const time::time_t &time) {
	RestoreResult result;
	if (state == nullptr or state->get_map() == nullptr) {
		result.error = "Kein Spiel";
		return result;
	}
	if (data.generated_first != generated.first or data.generated_last != generated.second) {
		result.error = "Karte passt nicht zum Spielstand (Objekte " + std::to_string(generated.first) + ".."
		               + std::to_string(generated.second) + " statt " + std::to_string(data.generated_first) + ".."
		               + std::to_string(data.generated_last) + ")";
		return result;
	}
	auto map = state->get_map();
	auto combat = state->get_combat();
	const double now = time.to_double();

	// XR fork W7: the stances of the file replace the teams the map started with
	if (auto combat_state = state->get_combat()) {
		const auto started = combat_state->get_stances();
		for (const auto &[pair, stance] : started) {
			combat_state->set_stance(pair.first, pair.second, diplomacy::stance_t::ENEMY);
		}
	}

	// players
	for (const auto &p : data.players) {
		if (state->has_player(static_cast<player_id_t>(p.id))) {
			for (const auto &[other, word] : p.stances) {
				diplomacy::stance_t stance;
				if (state->get_combat() and diplomacy::parse_stance(word, stance)) {
					state->get_combat()->set_stance(static_cast<player_id_t>(p.id), static_cast<player_id_t>(other), stance);
				}
			}
			state->get_player(static_cast<player_id_t>(p.id))->get_resources().set(p.resources);
			auto vision = state->get_vision();
			if (not p.explored.empty() and vision and not vision->set_explored_text(static_cast<player_id_t>(p.id), p.explored)) {
				log::log(WARN << "Load: explored tiles of player " << p.id << " do not fit the map, dropped");
			}
			// XR fork W3: queued farm reseeds
			if (auto food_state = state->get_food()) {
				food_state->set_reseeds(static_cast<player_id_t>(p.id), p.reseeds);
			}
		}
		else {
			log::log(WARN << "Load: player " << p.id << " does not exist, resources dropped");
		}
	}

	// XR fork W1: the civilisation came with the map (MapSettings::civs); researched technologies are
	// applied again in their order (patches + refresh of the generated entities), before the saved
	// health of the entities is restored
	if (auto techs = state->get_tech()) {
		size_t applied = 0;
		for (const auto &p : data.players) {
			const auto id = static_cast<player_id_t>(p.id);
			if (not state->has_player(id)) {
				continue;
			}
			const auto *current = techs->find(id);
			if (not p.civ.empty() and (current == nullptr or current->civ != p.civ)) {
				log::log(WARN << "Load: player " << p.id << " played " << p.civ << ", the map gave "
				              << (current == nullptr or current->civ.empty() ? std::string{"none"} : current->civ));
			}
			for (const auto &b : p.built) {
				techs->building_done(id, b);
			}
			for (const auto &t : p.techs) {
				applied += techs->research_done(state, loop, id, t, time, false) ? 1 : 0;
			}
		}
		if (applied > 0) {
			log::log(INFO << "Load: " << applied << " researched technologies applied");
		}
	}

	// gone objects
	for (auto id : data.removed) {
		auto entity = find_entity(state, id);
		if (entity == nullptr) {
			continue;
		}
		remove_now(state, entity, time);
		++result.removed;
	}

	// entities: generated ones in place, the others created again (ids mapped)
	std::unordered_map<entity_id_t, entity_id_t> id_map;
	std::vector<std::pair<const SavedEntity *, std::shared_ptr<GameEntity>>> restored;
	for (const auto &e : data.entities) {
		std::shared_ptr<GameEntity> entity;
		if (e.generated) {
			entity = find_entity(state, e.id);
			if (entity == nullptr) {
				log::log(WARN << "Load: generated object " << e.id << " (" << e.fqon << ") missing, skipped");
				++result.skipped;
				continue;
			}
			++result.updated;
		}
		else {
			if (not state->has_player(static_cast<player_id_t>(e.owner))) {
				log::log(WARN << "Load: entity " << e.id << " of unknown player " << e.owner << ", skipped");
				++result.skipped;
				continue;
			}
			try {
				entity = factory->add_game_entity(loop, state, static_cast<player_id_t>(e.owner), e.fqon);
			}
			catch (std::exception &err) {
				log::log(WARN << "Load: entity " << e.id << " (" << e.fqon << ") not creatable: " << err.what());
				++result.skipped;
				continue;
			}
			auto owner = component_of<component::Ownership>(entity, component::component_t::OWNERSHIP);
			auto position = component_of<component::Position>(entity, component::component_t::POSITION);
			auto activity = component_of<component::Activity>(entity, component::component_t::ACTIVITY);
			if (owner == nullptr or position == nullptr or activity == nullptr) {
				log::log(WARN << "Load: entity " << e.id << " (" << e.fqon << ") lacks components, skipped");
				++result.skipped;
				continue;
			}
			owner->set_owner(time, static_cast<player_id_t>(e.owner));
			position->set_position(time, coord::phys3{coord::phys_t{e.ne}, coord::phys_t{e.se}, coord::phys_t{e.up}});
			position->set_angle(time, coord::phys_angle_t{e.angle});
			activity->init(time);
			entity->get_manager()->run_activity_system(time);
			state->add_game_entity(entity);
			++result.created;
		}
		id_map[e.id] = entity->get_id();
		restored.emplace_back(&e, entity);
	}

	// state of each entity
	for (auto &[e, entity] : restored) {
		auto position = component_of<component::Position>(entity, component::component_t::POSITION);
		const coord::phys3 pos{coord::phys_t{e->ne}, coord::phys_t{e->se}, coord::phys_t{e->up}};
		if (e->generated) {
			position->set_position(time, pos);
			position->set_angle(time, coord::phys_angle_t{e->angle});
			// XR fork W3: herdables change their owner during the game
			auto owner = component_of<component::Ownership>(entity, component::component_t::OWNERSHIP);
			if (owner != nullptr and owner->get_owners().get(time) != static_cast<player_id_t>(e->owner)
			    and state->has_player(static_cast<player_id_t>(e->owner))) {
				owner->set_owner(time, static_cast<player_id_t>(e->owner));
				if (not entity->get_animation_path().empty()) {
					entity->render_update(time, entity->get_animation_path());
				}
			}
		}
		if (auto harvestable = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE)) {
			if (e->harvest) {
				harvestable->set_amount(*e->harvest);
			}
		}
		if (auto gather = component_of<component::Gather>(entity, component::component_t::GATHER)) {
			if (e->carried > 0.0) {
				gather->set_carried(static_cast<resource_t>(std::clamp(e->carried_type, 0, static_cast<int>(RESOURCE_COUNT) - 1)),
				                    e->carried);
			}
		}
		auto constructable = component_of<component::Constructable>(entity, component::component_t::CONSTRUCTABLE);
		auto spot = component_of<component::Harvestable>(entity, component::component_t::HARVESTABLE);
		const bool field = spot != nullptr and food::is_field(spot->kind);
		if (constructable != nullptr and not e->generated) {
			// XR fork W5 (version 6): a gate covers its line of 4 tiles; open when built and not locked
			if (e->gate_dir and state->get_buildings()) {
				building::Gate gate;
				gate.owner = static_cast<player_id_t>(e->owner);
				gate.dir = static_cast<building::gate_dir_t>(*e->gate_dir);
				gate.anchor = building::gate_anchor(e->ne, e->se, gate.dir);
				gate.locked = e->gate_locked;
				gate.finished = not(e->construction and *e->construction < 1.0);
				state->get_buildings()->add_gate(entity->get_id(), gate);
				if (not gate.finished or gate.locked) {
					for (const auto &tile : state->get_buildings()->footprint(entity->get_id()).value_or(std::vector<coord::tile>{})) {
						map->block_tile(tile, time);
					}
				}
			}
			// buildings block their tiles (generated ones already do; XR fork W3: farms and traps never)
			else if (not field) {
				for (const auto &tile : building_footprint(pos, constructable->radius)) {
					map->block_tile(tile, time);
				}
			}
			if (e->construction and *e->construction < 1.0) {
				constructable->start_foundation(e->build_time);
				constructable->set_progress(*e->construction);
				prod::update_building_health(state, entity, -1.0, time);
				if (entity->has_component(component::component_t::DROP_SITE)) {
					constructable->pending_drop_site = entity->get_component(component::component_t::DROP_SITE);
					entity->remove_component(component::component_t::DROP_SITE);
				}
				if (not constructable->construct_animation.empty()) {
					entity->render_update(time, constructable->construct_animation);
				}
				if (production != nullptr) {
					production->track_foundation(entity->get_id());
				}
			}
		}
		if (e->health) {
			set_health(state, entity, *e->health, time);
		}
		// XR fork W3: a carcass dies again (the food state keeps it with its saved food)
		if (e->dead and combat != nullptr and spot != nullptr and not combat->is_dead(entity->get_id())) {
			combat->kill(state, entity->get_id(), time);
		}
		if (auto queue = component_of<component::ProductionQueue>(entity, component::component_t::PRODUCTION_QUEUE)) {
			for (const auto &q : e->queue) {
				prod::TrainQueue::Item item;
				item.name = q.name;
				item.fqon = q.fqon;
				item.cost = q.cost;
				item.time = q.time;
				item.started = q.remaining >= 0.0 ? now - (q.time - q.remaining) : -1.0;
				item.research = q.research;
				queue->get_queue().restore(item);
			}
			if (e->rally) {
				queue->rally_point = map->on_terrain(
					coord::phys3{coord::phys_t{(*e->rally)[0]}, coord::phys_t{(*e->rally)[1]}, coord::phys_t{0.0}});
				if (not e->rally_kind.empty()) {
					using rally_t = component::ProductionQueue::rally_t;
					component::ProductionQueue::RallyTarget rally;
					rally.kind = e->rally_kind == "resource" ? rally_t::RESOURCE
					           : e->rally_kind == "entity"   ? rally_t::ENTITY
					                                         : rally_t::ENEMY;
					auto it = id_map.find(e->rally_entity);
					rally.entity = it != id_map.end() ? it->second : e->rally_entity;
					rally.resource = static_cast<resource_t>(e->rally_resource);
					queue->rally_target = rally;
				}
			}
		}
	}

	// XR fork W4 (version 4): stances, formations, garrison, bell, orders of round W4 (all entities exist)
	size_t garrisoned = 0;
	if (combat != nullptr) {
		auto mapped = [&](uint64_t id) {
			auto it = id_map.find(id);
			return it != id_map.end() ? it->second : id;
		};
		for (auto &[e, entity] : restored) {
			if (e->stance.empty() and e->formation.empty() and e->combat_order.empty() and not e->garrison and not e->bell) {
				continue;
			}
			combat::SavedCombat s;
			s.stance = combat::stance_from_string(e->stance).value_or(combat::stance_t::AGGRESSIVE);
			s.formation = combat::formation_from_string(e->formation).value_or(combat::formation_t::NONE);
			if (not e->combat_order.empty()) {
				s.order = combat::order_from_string(e->combat_order).value_or(combat::order_t::NONE);
				if (s.order == combat::order_t::GUARD or s.order == combat::order_t::FOLLOW) {
					s.order_entity = mapped(e->combat_entity);
				}
				for (const auto &p : e->combat_route) {
					s.route.push_back({p[0], p[1]});
				}
			}
			if (e->garrison) {
				s.inside = mapped(*e->garrison);
				s.by_bell = e->garrison_bell;
				s.bell_from = {e->bell_ne, e->bell_se};
				if (e->resume == "build") {
					s.resume_build = mapped(e->resume_entity);
				}
				else if (e->resume == "gather") {
					s.resume_gather = mapped(e->resume_entity);
				}
			}
			s.bell = e->bell;
			combat->load_entity(state, entity->get_id(), s, time);
			garrisoned += combat->garrisoned_in(entity->get_id()) ? 1 : 0;
		}
	}
	if (garrisoned > 0) {
		log::log(INFO << "Load: " << garrisoned << " units back in their buildings");
	}

	// XR fork W5 (version 6): the rest of a wall drag; the order target stays in it, so that the build order
	// keeps the drag (system/build.cpp)
	for (auto &[e, entity] : restored) {
		auto builder = component_of<component::Builder>(entity, component::component_t::BUILDER);
		if (builder == nullptr or e->wall_queue.empty()) {
			continue;
		}
		auto mapped = [&](uint64_t id) {
			auto it = id_map.find(id);
			return it != id_map.end() ? it->second : static_cast<entity_id_t>(id);
		};
		builder->wall_queue.clear();
		for (auto id : e->wall_queue) {
			builder->wall_queue.push_back(mapped(id));
		}
		if (e->order == "build") {
			builder->wall_queue.push_back(mapped(e->order_target));
		}
	}

	// XR fork W6 (version 6): faith, conversions, relics, cargo, trebuchets (all entities exist)
	if (auto special_state = state->get_special()) {
		// XR fork W7 (version 7): prices of the shared market
		if (not data.market.empty() and not special_state->set_market_text(data.market)) {
			log::log(WARN << "Load: market prices not readable, base prices used");
		}
		auto mapped = [&](uint64_t id) -> entity_id_t {
			auto it = id_map.find(id);
			return it != id_map.end() ? it->second : id;
		};
		size_t special_count = 0;
		for (auto &[e, entity] : restored) {
			if (not e->faith and not e->convert_target and not e->relic_carry and e->relic_store.empty()
			    and e->cargo.empty() and e->siege_mode.empty() and not e->trade_home) {
				continue;
			}
			special::SavedSpecial s;
			s.faith = e->faith;
			if (e->convert_target) {
				s.convert_target = static_cast<entity_id_t>(*e->convert_target);
				s.convert_time = e->convert_time;
			}
			if (e->relic_carry) {
				s.relic_carry = static_cast<entity_id_t>(*e->relic_carry);
			}
			s.relic_store.assign(e->relic_store.begin(), e->relic_store.end());
			s.cargo.assign(e->cargo.begin(), e->cargo.end());
			s.siege_mode = e->siege_mode;
			if (e->trade_home) {
				s.trade_home = static_cast<entity_id_t>(*e->trade_home);
				s.trade_partner = static_cast<entity_id_t>(e->trade_partner);
				s.trade_to_partner = e->trade_to_partner;
				s.trade_goods = e->trade_goods;
			}
			special_state->load_entity(state, entity->get_id(), s, mapped, time);
			special_count += 1;
		}
		if (special_count > 0) {
			log::log(INFO << "Load: special state of " << special_count << " entities (faith, relics, cargo)");
		}
	}

	// orders (all entities exist now)
	for (auto &[e, entity] : restored) {
		if (e->order.empty() or (combat != nullptr and combat->garrisoned_in(entity->get_id()))) {
			continue;
		}
		auto queue = component_of<component::CommandQueue>(entity, component::component_t::COMMANDQUEUE);
		if (queue == nullptr) {
			continue;
		}
		std::shared_ptr<GameEntity> target;
		if (e->order != "move") {
			auto it = id_map.find(e->order_target);
			target = find_entity(state, it != id_map.end() ? it->second : e->order_target);
			if (target == nullptr) {
				log::log(INFO << "Load: " << e->order << " target of entity " << e->id << " is gone, unit idles");
				continue;
			}
		}
		if (e->order == "gather" and econ::can_gather_from(entity, target)) {
			queue->add_command(time, std::make_shared<component::command::GatherCommand>(target->get_id()));
			++result.orders;
		}
		else if (e->order == "build" and prod::can_build(entity, target, time)) {
			queue->add_command(time, std::make_shared<component::command::BuildCommand>(target->get_id()));
			++result.orders;
		}
		else if (e->order == "attack" and combat != nullptr) {
			if (combat->order_attack(state, entity->get_id(), target->get_id(), time)) {
				++result.orders;
			}
		}
		else if (e->order == "move") {
			queue->add_command(time, std::make_shared<component::command::MoveCommand>(
				                      map->on_terrain(coord::phys3{coord::phys_t{e->order_ne}, coord::phys_t{e->order_se}, coord::phys_t{0.0}})));
			++result.orders;
		}
	}

	result.ok = true;
	log::log(INFO << "Load: restored t=" << data.game_time << " s: " << result.updated << " map objects updated, "
	              << result.created << " entities created, " << result.removed << " removed, " << result.orders
	              << " orders, " << result.skipped << " skipped (" << data.title << ", " << data.created << ")");
	return result;
}

} // namespace openage::gamestate::save
