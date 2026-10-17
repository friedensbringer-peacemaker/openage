// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "tech.h"

#include <algorithm>
#include <exception>
#include <unordered_set>

#include "log/log.h"
#include "log/message.h"

#include "assets/mod_manager.h"
#include "gamestate/api/ability.h"
#include "gamestate/api/animation.h"
#include "gamestate/api/property.h"
#include "gamestate/api/types.h"
#include "gamestate/combat/combat_state.h"
#include "gamestate/component/api/builder.h"
#include "gamestate/component/api/constructable.h"
#include "gamestate/component/api/gather.h"
#include "gamestate/component/api/production_queue.h"
#include "gamestate/component/api_component.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/types.h"
#include "gamestate/econ.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/player.h"
#include "gamestate/production.h"
#include "gamestate/production_math.h"


namespace openage::gamestate::tech {

namespace {

std::string object_name(const nyan::ValueHolder &value) {
	auto obj = std::dynamic_pointer_cast<nyan::ObjectValue>(value.get_ptr());
	return obj ? obj->get_name() : std::string{};
}

std::string parent_short(const nyan::Object &obj) {
	const auto &parents = obj.get_parents();
	return parents.empty() ? std::string{} : short_of(parents[0]);
}

template <typename T>
std::shared_ptr<T> component_of(const std::shared_ptr<GameEntity> &entity, component::component_t type) {
	if (entity == nullptr or not entity->has_component(type)) {
		return nullptr;
	}
	return std::dynamic_pointer_cast<T>(entity->get_component(type));
}

player_id_t owner_of(const std::shared_ptr<GameEntity> &entity, const time::time_t &time) {
	auto owner = component_of<component::Ownership>(entity, component::component_t::OWNERSHIP);
	return owner ? owner->get_owners().get(time) : player_id_t{0};
}

/// nyan game entity of an entity, from the names of its abilities (like save_game.cpp)
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

resource_amounts_t cost_of(const std::shared_ptr<nyan::View> &view, const nyan::Object &cost) {
	resource_amounts_t result{};
	for (const auto &amount_val : cost.get_set("ResourceCost.amount")) {
		auto amount = view->get_object(object_name(amount_val));
		auto type = resource_from_name(amount.get_object("ResourceAmount.type").get_name());
		if (type) {
			result[static_cast<size_t>(*type)] += static_cast<double>(amount.get_int("ResourceAmount.amount"));
		}
	}
	return result;
}

Condition read_condition(const std::shared_ptr<nyan::View> &view, const nyan::Object &obj, int depth) {
	using k = Condition::kind_t;
	const auto type = parent_short(obj);
	Condition c;
	if (depth > 16) {
		c.kind = k::NEVER;
		c.unsupported = "too deep";
		return c;
	}
	try {
		if (type == "TechResearched") {
			return Condition::tech(obj.get_object("TechResearched.tech").get_name());
		}
		if (type == "GameEntityProgress") {
			auto status = obj.get_object("GameEntityProgress.progress_status");
			auto progress_type = short_of(status.get_object("ProgressStatus.progress_type").get_name());
			if (progress_type == "Construct" and status.get_float("ProgressStatus.progress") >= 100.0 - 1e-6) {
				return Condition::built(obj.get_object("GameEntityProgress.game_entity").get_name());
			}
		}
		else if (type == "AND" or type == "OR" or type == "NOT" or type == "SUBSETMIN" or type == "SUBSETMAX"
		         or type == "XOR" or type == "MULTIXOR") {
			std::vector<Condition> inputs;
			for (const auto &input_val : obj.get_set("LogicGate.inputs")) {
				inputs.push_back(read_condition(view, view->get_object(object_name(input_val)), depth + 1));
			}
			if (type == "AND") {
				return Condition::gate(k::AND, std::move(inputs));
			}
			if (type == "OR") {
				return Condition::gate(k::OR, std::move(inputs));
			}
			if (type == "NOT") {
				return Condition::gate(k::NOT, std::move(inputs));
			}
			if (type == "SUBSETMIN") {
				auto size = obj.get_int("SUBSETMIN.size");
				return Condition::gate(k::SUBSETMIN, std::move(inputs), static_cast<size_t>(std::max<int64_t>(0, size)));
			}
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Tech: condition " << obj.get_name() << " not readable: " << err.what());
	}
	c.kind = k::NEVER;
	c.unsupported = type;
	return c;
}

/// animation paths of an ability (raw as the systems use them, and with the shared-path fix)
std::vector<std::string> ability_animations(const nyan::Object &ability) {
	std::vector<std::string> out;
	if (not api::APIAbility::check_property(ability, api::ability_property_t::ANIMATED)) {
		return out;
	}
	auto property = api::APIAbility::get_property(ability, api::ability_property_t::ANIMATED);
	auto animations = api::APIAbilityProperty::get_animations(property);
	for (const auto &path : api::APIAnimation::get_animation_paths(animations)) {
		out.push_back(path);
	}
	return out;
}

} // namespace


Condition read_condition_set(const std::shared_ptr<nyan::View> &view, const nyan::set_t &set) {
	std::vector<Condition> parts;
	for (const auto &value : set) {
		parts.push_back(read_condition(view, view->get_object(object_name(value)), 0));
	}
	return all_of(std::move(parts));
}


PlayerTech &TechState::player(player_id_t id) {
	return this->states[id];
}

const PlayerTech *TechState::find(player_id_t id) const {
	auto it = this->states.find(id);
	return it == this->states.end() ? nullptr : &it->second;
}

std::vector<player_id_t> TechState::players() const {
	std::vector<player_id_t> out;
	for (const auto &[id, s] : this->states) {
		out.push_back(id);
	}
	return out;
}

uint64_t TechState::version() const {
	return this->data_version;
}

std::vector<Message> TechState::take_messages() {
	std::vector<Message> out;
	out.swap(this->messages);
	return out;
}

bool TechState::is_researched(player_id_t player, const nyan::fqon_t &tech) const {
	const auto *s = this->find(player);
	return s != nullptr and s->facts.researched.contains(tech);
}

void TechState::building_done(player_id_t player, const nyan::fqon_t &building) {
	auto &s = this->player(player);
	if (s.facts.built.insert(building).second) {
		this->data_version += 1;
	}
}

std::string TechState::unit_label(player_id_t player, const nyan::fqon_t &entity) const {
	const auto name = short_of(entity);
	if (const auto *s = this->find(player)) {
		// the last researched upgrade of the entity's line names it
		for (auto it = s->order.rbegin(); it != s->order.rend(); ++it) {
			if (in_line(entity, *it)) {
				return prod::label_of(short_of(*it), true);
			}
		}
	}
	return prod::label_of(name);
}

size_t TechState::apply_patches(const std::shared_ptr<GameState> &state,
                                player_id_t player,
                                const std::vector<nyan::Object> &wrappers,
                                const std::string &what) {
	auto view = state->get_player(player)->get_db_view();
	auto tx = view->new_transaction(this->next_order++);
	size_t added = 0;
	size_t failed = 0;
	for (const auto &wrapper : wrappers) {
		try {
			if (wrapper.is_patch()) {
				added += tx.add(wrapper) ? 1 : 0;
				continue;
			}
			auto patch = wrapper.get_object("Patch.patch");
			if (tx.add(patch)) {
				added += 1;
			}
			else {
				failed += 1;
			}
		}
		catch (std::exception &err) {
			failed += 1;
			log::log(WARN << "Tech: patch " << wrapper.get_name() << " of " << what << " not applied: " << err.what());
		}
	}
	bool ok = tx.commit();
	if (not ok) {
		log::log(ERR << "Tech: transaction of " << what << " for player " << player << " failed");
		return 0;
	}
	this->data_version += 1;
	this->research_cache.clear();
	log::log(INFO << "Tech: player " << player << " applied " << what << " (" << added << " patches"
	              << (failed > 0 ? ", " + std::to_string(failed) + " failed" : std::string{}) << ")");
	return added;
}

std::map<nyan::fqon_t, std::vector<std::string>> TechState::animations_of(const std::shared_ptr<GameState> &state,
                                                                         player_id_t player,
                                                                         const time::time_t &time) const {
	std::map<nyan::fqon_t, std::vector<std::string>> out;
	auto view = state->get_player(player)->get_db_view();
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (not entity->has_component(component::component_t::OWNERSHIP) or owner_of(entity, time) != player) {
			continue;
		}
		auto fqon = fqon_of(entity);
		if (fqon.empty() or out.contains(fqon)) {
			continue;
		}
		auto &paths = out[fqon];
		try {
			auto obj = view->get_object(fqon);
			// ability names in a fixed order (sorted), so that before and after line up
			std::vector<std::string> abilities;
			for (const auto &value : obj.get_set("GameEntity.abilities")) {
				abilities.push_back(object_name(value));
			}
			std::sort(abilities.begin(), abilities.end());
			for (const auto &name : abilities) {
				std::string path;
				try {
					auto list = ability_animations(view->get_object(name));
					path = list.empty() ? std::string{} : list.front();
				}
				catch (std::exception &) {
					// abilities without properties (Collision, ...)
				}
				paths.push_back(name + "=" + path);
			}
		}
		catch (std::exception &err) {
			log::log(DBG << "Tech: animations of " << fqon << ": " << err.what());
		}
	}
	return out;
}

void TechState::refresh_player(const std::shared_ptr<GameState> &state,
                               const std::shared_ptr<openage::event::EventLoop> &loop,
                               player_id_t player,
                               const std::map<nyan::fqon_t, std::vector<std::string>> &animations_before,
                               bool proportional,
                               const time::time_t &time) {
	auto view = state->get_player(player)->get_db_view();
	auto combat = state->get_combat();
	if (combat) {
		combat->invalidate_stats(view);
	}
	auto after = this->animations_of(state, player, time);
	// changed animations per entity type: old path -> new path
	std::map<nyan::fqon_t, std::vector<std::pair<std::string, std::string>>> changed;
	for (const auto &[fqon, now_paths] : after) {
		auto it = animations_before.find(fqon);
		if (it == animations_before.end() or it->second.size() != now_paths.size()) {
			continue;
		}
		for (size_t i = 0; i < now_paths.size(); ++i) {
			if (it->second[i] == now_paths[i]) {
				continue;
			}
			auto old_path = it->second[i].substr(it->second[i].find('=') + 1);
			auto new_path = now_paths[i].substr(now_paths[i].find('=') + 1);
			if (not old_path.empty() and not new_path.empty()) {
				changed[fqon].emplace_back(old_path, new_path);
				log::log(INFO << "Tech: player " << player << " " << short_of(fqon) << " animation " << old_path
				              << " -> " << new_path);
			}
		}
	}

	size_t entities = 0;
	size_t health = 0;
	size_t animations = 0;
	std::vector<std::shared_ptr<GameEntity>> own;
	for (const auto &[id, entity] : state->get_game_entities()) {
		if (entity->has_component(component::component_t::OWNERSHIP) and owner_of(entity, time) == player
		    and not(combat and combat->is_dead(id))) {
			own.push_back(entity);
		}
	}
	std::sort(own.begin(), own.end(), [](const auto &a, const auto &b) { return a->get_id() < b->get_id(); });
	for (const auto &entity : own) {
		auto fqon = fqon_of(entity);
		if (fqon.empty()) {
			continue;
		}
		entities += 1;
		// the components as a new entity would get them, data copied into the running ones
		auto fresh = std::make_shared<GameEntity>(entity->get_id());
		try {
			econ::init_components(loop, view, fresh, fqon);
			prod::init_components(loop, view, fresh, fqon);
		}
		catch (std::exception &err) {
			log::log(WARN << "Tech: refresh of " << fqon << " (entity " << entity->get_id() << ") failed: " << err.what());
			continue;
		}
		auto gather = component_of<component::Gather>(entity, component::component_t::GATHER);
		auto fresh_gather = component_of<component::Gather>(fresh, component::component_t::GATHER);
		if (gather and fresh_gather) {
			for (size_t r = 0; r < RESOURCE_COUNT; ++r) {
				auto type = static_cast<resource_t>(r);
				if (fresh_gather->can_gather(type)) {
					gather->set_skill(type, fresh_gather->get_skill(type));
				}
			}
		}
		auto queue = component_of<component::ProductionQueue>(entity, component::component_t::PRODUCTION_QUEUE);
		auto fresh_queue = component_of<component::ProductionQueue>(fresh, component::component_t::PRODUCTION_QUEUE);
		if (queue and fresh_queue) {
			auto creatables = fresh_queue->get_creatables();
			queue->set_creatables(std::move(creatables));
		}
		else if (fresh_queue and not queue) {
			// a patch gave the building something to train or research
			entity->add_component(fresh_queue);
		}
		auto builder = component_of<component::Builder>(entity, component::component_t::BUILDER);
		auto fresh_builder = component_of<component::Builder>(fresh, component::component_t::BUILDER);
		if (builder and fresh_builder) {
			auto buildables = fresh_builder->get_buildables();
			builder->set_buildables(std::move(buildables));
			builder->animation = fresh_builder->animation;
		}
		auto constructable = component_of<component::Constructable>(entity, component::component_t::CONSTRUCTABLE);
		auto fresh_constructable = component_of<component::Constructable>(fresh, component::component_t::CONSTRUCTABLE);
		bool building_site = false;
		if (constructable and fresh_constructable) {
			constructable->population = fresh_constructable->population;
			constructable->construct_animation = fresh_constructable->construct_animation;
			building_site = not constructable->is_complete();
			if (constructable->max_health != fresh_constructable->max_health and building_site) {
				// keep the share of the construction (AoE II: the site grows to the new maximum)
				constructable->max_health = fresh_constructable->max_health;
				prod::update_building_health(state, entity, constructable->get_progress(), time);
			}
			constructable->max_health = fresh_constructable->max_health;
		}
		if (combat and combat->refresh_entity(entity, view, fqon, building_site, proportional, time)) {
			health += 1;
		}
		// animations of the new stage/age
		auto it = changed.find(fqon);
		if (it != changed.end()) {
			const auto &current = entity->get_animation_path();
			for (const auto &[old_path, new_path] : it->second) {
				if (current == old_path or current == prod::sprite_path(old_path)) {
					entity->render_update(time, current == old_path ? new_path : prod::sprite_path(new_path));
					animations += 1;
					break;
				}
			}
		}
	}
	log::log(INFO << "Tech: player " << player << " refreshed " << entities << " entities (" << health
	              << " with new maximum health, " << animations << " new animations)");
}

bool TechState::apply_civ(const std::shared_ptr<GameState> &state,
                          const std::shared_ptr<openage::event::EventLoop> &loop,
                          player_id_t player,
                          const std::string &civ,
                          const time::time_t &time) {
	const int index = prod::civ_index(civ);
	if (index < 0 or not state->has_player(player)) {
		log::log(WARN << "Tech: unknown civilisation '" << civ << "' for player " << player);
		return false;
	}
	auto view = state->get_player(player)->get_db_view();
	std::optional<nyan::Object> setup;
	if (auto mods = state->get_mod_manager()) {
		for (const auto &modpack : mods->get_load_order()) {
			try {
				setup = view->get_object(prod::civ_fqon(modpack, prod::CIVS[static_cast<size_t>(index)]));
				break;
			}
			catch (std::exception &) {
			}
		}
	}
	if (not setup) {
		log::log(WARN << "Tech: civilisation " << civ << " not in the data");
		return false;
	}
	auto before = this->animations_of(state, player, time);
	std::vector<nyan::Object> wrappers;
	size_t skipped = 0;
	try {
		for (const auto &value : setup->get_orderedset("PlayerSetup.game_setup")) {
			auto wrapper = view->get_object(object_name(value));
			// the converted HD data gives the town centre of the architecture sets only 3 of its 8 sprite
			// parts (no deltas): it would be drawn as rubble – keep the generic town centre (converter, W0)
			try {
				auto patch = wrapper.get_object("Patch.patch");
				const auto *target = patch.get_target();
				const std::string tc_idle = ".TownCenter.Idle.Animated";
				if (target != nullptr and target->size() > tc_idle.size()
				    and target->compare(target->size() - tc_idle.size(), tc_idle.size(), tc_idle) == 0) {
					skipped += 1;
					continue;
				}
			}
			catch (std::exception &) {
			}
			wrappers.push_back(wrapper);
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Tech: game setup of " << civ << " not readable: " << err.what());
	}
	if (skipped > 0) {
		log::log(INFO << "Tech: civilisation " << civ << ": " << skipped
		              << " town centre graphic patch skipped (incomplete sprite in the converted data)");
	}
	this->apply_patches(state, player, wrappers, "civilisation " + civ);
	// starting resources of the data come on top of the standard start (they are 0 in AoE II)
	try {
		auto &stock = state->get_player(player)->get_resources();
		for (const auto &value : setup->get_set("PlayerSetup.starting_resources")) {
			auto amount = view->get_object(object_name(value));
			auto type = resource_from_name(amount.get_object("ResourceAmount.type").get_name());
			auto n = static_cast<double>(amount.get_int("ResourceAmount.amount"));
			if (type and n != 0.0) {
				stock.add(*type, n);
			}
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Tech: starting resources of " << civ << " not readable: " << err.what());
	}
	auto &s = this->player(player);
	s.civ = civ;
	this->data_version += 1;
	this->refresh_player(state, loop, player, before, false, time);
	log::log(INFO << "Tech: player " << player << " plays " << civ << " (" << prod::civ_label(civ) << ")");
	return true;
}

bool TechState::research_done(const std::shared_ptr<GameState> &state,
                              const std::shared_ptr<openage::event::EventLoop> &loop,
                              player_id_t player,
                              const nyan::fqon_t &tech,
                              const time::time_t &time,
                              bool announce) {
	if (not state->has_player(player) or this->is_researched(player, tech)) {
		return false;
	}
	auto view = state->get_player(player)->get_db_view();
	std::vector<nyan::Object> wrappers;
	try {
		auto obj = view->get_object(tech);
		for (const auto &value : obj.get_orderedset("Tech.updates")) {
			wrappers.push_back(view->get_object(object_name(value)));
		}
	}
	catch (std::exception &err) {
		log::log(WARN << "Tech: technology " << tech << " not readable: " << err.what());
	}
	auto before = this->animations_of(state, player, time);
	this->apply_patches(state, player, wrappers, "technology " + short_of(tech));
	auto &s = this->player(player);
	s.order.push_back(tech);
	s.facts.researched.insert(tech);
	const int age = age_of_tech(short_of(tech));
	if (age > s.age) {
		s.age = age;
	}
	this->data_version += 1;
	// line upgrades and ages replace types (health scaled), other technologies change attributes
	this->refresh_player(state, loop, player, before, age > 0 or not line_prefix(tech).empty(), time);
	log::log(INFO << "Tech: player " << player << " researched " << short_of(tech) << " at t=" << time.to_double()
	              << " s" << (age > 0 ? std::string{", age "} + age_label(age) : std::string{}));
	if (announce) {
		this->messages.push_back({player, short_of(tech), age});
	}
	return true;
}

const std::vector<Researchable> &TechState::researchables(const std::shared_ptr<GameState> &state,
                                                          player_id_t player,
                                                          const nyan::fqon_t &building) {
	auto key = std::make_pair(player, building);
	auto it = this->research_cache.find(key);
	if (it != this->research_cache.end()) {
		return it->second;
	}
	std::vector<Researchable> out;
	if (state->has_player(player)) {
		auto view = state->get_player(player)->get_db_view();
		try {
			auto obj = view->get_object(building);
			for (const auto &ability_val : obj.get_set("GameEntity.abilities")) {
				auto ability = view->get_object(object_name(ability_val));
				if (ability.get_parents()[0] != "engine.ability.type.Research") {
					continue;
				}
				for (const auto &r_val : ability.get_set("Research.researchables")) {
					try {
						auto r = view->get_object(object_name(r_val));
						Researchable item;
						item.fqon = r.get_object("ResearchableTech.tech").get_name();
						item.name = short_of(item.fqon);
						item.cost = cost_of(view, r.get_object("ResearchableTech.cost"));
						item.time = r.get_float("ResearchableTech.research_time");
						item.condition = read_condition_set(view, r.get_set("ResearchableTech.condition"));
						out.push_back(std::move(item));
					}
					catch (std::exception &err) {
						log::log(WARN << "Tech: researchable " << object_name(r_val) << " not readable: " << err.what());
					}
				}
			}
		}
		catch (std::exception &err) {
			log::log(WARN << "Tech: research of " << building << " not readable: " << err.what());
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const Researchable &a, const Researchable &b) {
		return prod::display_order(a.name, true) < prod::display_order(b.name, true);
	});
	return this->research_cache.emplace(key, std::move(out)).first->second;
}

} // namespace openage::gamestate::tech
