// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "vision_state.h"

#include <algorithm>
#include <chrono>

#include "event/event_loop.h"
#include "log/log.h"
#include "log/message.h"

#include "gamestate/combat/combat_state.h"
#include "gamestate/combat/stats.h"
#include "gamestate/component/api/harvestable.h"
#include "gamestate/component/internal/ownership.h"
#include "gamestate/component/internal/position.h"
#include "gamestate/component/types.h"
#include "gamestate/game_entity.h"
#include "gamestate/game_state.h"
#include "gamestate/map.h"


namespace openage::gamestate::vision {


VisionState::VisionState(const std::shared_ptr<openage::event::EventLoop> &loop) :
	EventEntity{loop},
	loop{loop},
	handler{std::make_shared<VisionEventHandler>()} {
}

size_t VisionState::id() const {
	// fixed id, the vision exists once per game ("visi")
	return 0x76697369;
}

std::string VisionState::idstr() const {
	return "vision";
}

void VisionState::start(const std::shared_ptr<GameState> &state,
                        vision_mode_t mode,
                        const std::unordered_set<player_id_t> &neutral,
                        const time::time_t &time) {
	if (this->started or not state->get_map()) {
		return;
	}
	this->mode = mode;
	this->neutral = neutral;
	size_t players = 0;
	while (state->has_player(static_cast<player_id_t>(players))) {
		players += 1;
	}
	this->grid_players = players;
	const auto &size = state->get_map()->get_size();
	this->grid = VisionGrid{size[0], size[1], players};
	this->memory.assign(players, {});
	if (mode == vision_mode_t::EXPLORED) {
		for (size_t p = 0; p < players; ++p) {
			this->grid.explore_all(p);
		}
	}
	this->started = true;
	log::log(INFO << "Vision: mode " << vision_mode_name(mode) << ", map " << size[0] << "x" << size[1] << ", "
	              << players << " players (tick " << TICK << " s)");
	this->tick(state, time);
}

void VisionState::set_terrain_colors(std::vector<uint32_t> &&colors) {
	this->terrain = std::make_shared<const std::vector<uint32_t>>(std::move(colors));
}

void VisionState::set_view_player(player_id_t player) {
	this->view_player = player;
}

bool VisionState::is_started() const {
	return this->started;
}

vision_mode_t VisionState::get_mode() const {
	return this->mode;
}

bool VisionState::neutral_player(player_id_t player) const {
	return this->neutral.contains(player);
}

bool VisionState::sees(player_id_t player, double ne, double se, double radius) const {
	if (not this->started or this->mode == vision_mode_t::ALL or this->neutral_player(player)
	    or player >= this->grid_players) {
		return true;
	}
	return this->grid.footprint_state(player, ne, se, radius) == tile_state_t::VISIBLE;
}

tile_state_t VisionState::footprint_state(player_id_t player, double ne, double se, double radius) const {
	if (not this->started or this->mode == vision_mode_t::ALL or this->neutral_player(player)
	    or player >= this->grid_players) {
		return tile_state_t::VISIBLE;
	}
	return this->grid.footprint_state(player, ne, se, radius);
}

bool VisionState::remembers(player_id_t player, entity_id_t id) const {
	return player < this->memory.size() and this->memory[player].contains(id);
}

void VisionState::remember(player_id_t player, entity_id_t id, double ne, double se, double radius) {
	if (player < this->memory.size()) {
		this->memory[player][id] = Place{ne, se, radius};
	}
}

bool VisionState::knows(player_id_t player, entity_id_t id, double ne, double se, double radius, bool building) const {
	return this->sees(player, ne, se, radius) or (building and this->remembers(player, id));
}

std::string VisionState::explored_text(player_id_t player) const {
	return this->grid.explored_text(player);
}

bool VisionState::set_explored_text(player_id_t player, const std::string &text) {
	return this->grid.set_explored_text(player, text);
}

const VisionGrid &VisionState::get_grid() const {
	return this->grid;
}

std::shared_ptr<const Snapshot> VisionState::snapshot() const {
	std::lock_guard<std::mutex> lock{this->shared_mutex};
	return this->published;
}

void VisionState::tick(const std::shared_ptr<GameState> &state, const time::time_t &time) {
	if (not this->started) {
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	auto combat = state->get_combat();
	const bool fog = this->mode != vision_mode_t::ALL;

	// 1. sight circles: restamp entities that moved to another tile (or changed owner/range)
	std::unordered_map<entity_id_t, Stamp> next_stamps;
	next_stamps.reserve(this->stamps.size() + 16);
	size_t restamped = 0;
	std::vector<EntityInfo> movers;
	std::vector<const EntityInfo *> buildings;
	this->tick_count += 1;
	// static entities seen this tick: count and id sum detect added/removed ones (static layer)
	size_t static_seen = 0;
	uint64_t static_sum = 0;
	bool static_changed = false;
	for (const auto &[id, entity] : state->get_game_entities()) {
		// entities that cannot move are read once and refreshed once a second (spread over the ticks)
		auto cached = this->static_cache.find(id);
		const bool fresh = cached != this->static_cache.end() and (id + this->tick_count) % 10 != 0;
		if (fresh and this->neutral_player(cached->second.owner)) {
			// gaia objects (trees, mines, bushes): nothing to stamp, no combat
			static_seen += 1;
			static_sum += id;
			continue;
		}
		// dead, or garrisoned in a building (XR fork W4: no sight of its own, not on the minimap)
		if (combat and (combat->is_dead(id) or combat->garrisoned_in(id))) {
			continue;
		}
		auto stats = combat ? combat->get_stats(id) : nullptr;
		const EntityInfo *info = nullptr;
		EntityInfo read;
		if (fresh) {
			info = &cached->second;
		}
		else {
			if (not entity->has_component(component::component_t::POSITION)
			    or not entity->has_component(component::component_t::OWNERSHIP)) {
				continue;
			}
			auto position = std::dynamic_pointer_cast<component::Position>(
				entity->get_component(component::component_t::POSITION));
			auto ownership = std::dynamic_pointer_cast<component::Ownership>(
				entity->get_component(component::component_t::OWNERSHIP));
			const auto pos = position->get_positions().get(time);
			const auto owner = ownership->get_owners().get(time);

			read.id = id;
			read.owner = owner;
			read.ne = pos.ne.to_double();
			read.se = pos.se.to_double();
			read.movable = stats and stats->movable;
			read.building = stats and stats->building;
			read.military = stats and stats->can_attack and stats->movable and not stats->villager;
			if (read.building) {
				read.radius = stats->radius;
				read.kind = dot_kind_t::BUILDING;
			}
			else if (stats and stats->unit and not this->neutral_player(owner)) {
				read.kind = dot_kind_t::UNIT;
			}
			else if (entity->has_component(component::component_t::HARVESTABLE)) {
				auto spot = std::dynamic_pointer_cast<component::Harvestable>(
					entity->get_component(component::component_t::HARVESTABLE));
				read.radius = read.movable ? 0.0 : std::min(spot->radius, 1.5);
				switch (spot->get_resource()) {
				case resource_t::WOOD:
					read.kind = dot_kind_t::TREE;
					break;
				case resource_t::GOLD:
					read.kind = dot_kind_t::GOLD;
					break;
				case resource_t::STONE:
					read.kind = dot_kind_t::STONE;
					break;
				case resource_t::FOOD:
				default:
					read.kind = read.movable ? dot_kind_t::ANIMAL : dot_kind_t::FOOD;
					break;
				}
			}
			else {
				read.kind = read.movable ? dot_kind_t::ANIMAL : dot_kind_t::OTHER;
			}
			if (read.movable) {
				if (this->static_cache.erase(id) > 0) {
					static_changed = true;
				}
				movers.push_back(read);
				info = &movers.back();
			}
			else {
				auto &slot = this->static_cache[id];
				if (slot.id != read.id or slot.owner != read.owner or slot.ne != read.ne or slot.se != read.se
				    or slot.radius != read.radius or slot.kind != read.kind or slot.building != read.building) {
					static_changed = true;
				}
				slot = read;
				info = &slot;
			}
		}
		if (not info->movable) {
			static_seen += 1;
			static_sum += id;
			if (info->building) {
				buildings.push_back(info);
			}
		}
		const auto owner = info->owner;
		if (not fog or not stats or owner >= this->grid_players or this->neutral_player(owner)) {
			continue;
		}
		Stamp stamp{owner, tile_of(info->ne), tile_of(info->se), quantize_radius(stats->line_of_sight)};
		auto old = this->stamps.find(id);
		if (old == this->stamps.end()) {
			this->grid.stamp(stamp.player, stamp.ne, stamp.se, stamp.q, +1);
			restamped += 1;
		}
		else {
			const auto &o = old->second;
			if (o.player != stamp.player or o.ne != stamp.ne or o.se != stamp.se or o.q != stamp.q) {
				this->grid.stamp(o.player, o.ne, o.se, o.q, -1);
				this->grid.stamp(stamp.player, stamp.ne, stamp.se, stamp.q, +1);
				restamped += 1;
			}
		}
		next_stamps.emplace(id, stamp);
	}
	// entities that died or left the game take their sight with them
	for (const auto &[id, o] : this->stamps) {
		if (not next_stamps.contains(id)) {
			this->grid.stamp(o.player, o.ne, o.se, o.q, -1);
			restamped += 1;
		}
	}
	this->stamps = std::move(next_stamps);
	if (static_seen != this->static_seen or static_sum != this->static_sum) {
		// static entities left the game (gathered empty, destroyed): drop them from the cache
		const auto &entities = state->get_game_entities();
		std::erase_if(this->static_cache, [&](const auto &entry) {
			return not entities.contains(entry.first) or (combat and combat->is_dead(entry.first));
		});
		this->static_seen = static_seen;
		this->static_sum = static_sum;
		static_changed = true;
	}
	this->restamps += restamped;

	// 2. remembered foreign buildings: seen ones are added, gone ones are forgotten once
	//    their place is in sight again
	const player_id_t view = this->view_player.load();
	bool view_memory_changed = false;
	if (fog) {
		std::unordered_set<entity_id_t> building_ids;
		for (const auto *b : buildings) {
			building_ids.insert(b->id);
		}
		for (size_t p = 0; p < this->memory.size(); ++p) {
			const auto player = static_cast<player_id_t>(p);
			if (this->neutral_player(player)) {
				continue;
			}
			auto &mem = this->memory[p];
			const size_t before = mem.size();
			bool added = false;
			for (const auto *b : buildings) {
				if (b->owner != player and not this->neutral_player(b->owner) and not mem.contains(b->id)
				    and this->grid.footprint_state(p, b->ne, b->se, b->radius) == tile_state_t::VISIBLE) {
					mem[b->id] = Place{b->ne, b->se, b->radius};
					added = true;
				}
			}
			std::erase_if(mem, [&](const auto &entry) {
				return not building_ids.contains(entry.first)
				       and this->grid.footprint_state(p, entry.second.ne, entry.second.se, entry.second.radius)
				               == tile_state_t::VISIBLE;
			});
			if (player == view and (added or mem.size() != before)) {
				view_memory_changed = true;
			}
		}
	}

	// 3. view of the local player
	auto snap = std::make_shared<Snapshot>();
	snap->mode = this->mode;
	snap->view_player = view;
	snap->neutral_player = this->neutral.empty() ? this->grid_players : *std::min_element(this->neutral.begin(), this->neutral.end());
	snap->ne_size = this->grid.ne_size();
	snap->se_size = this->grid.se_size();
	snap->game_time = time.to_double();
	snap->terrain = this->terrain;
	const bool view_fog = fog and view < this->grid_players and not this->neutral_player(view);
	bool tiles_changed = false;
	if (view_fog) {
		if (this->grid.revision() != this->tiles_grid_revision or view != this->tiles_player or not this->tiles) {
			auto tiles = std::make_shared<std::vector<uint8_t>>();
			this->grid.fill_states(view, *tiles);
			// the grid revision also counts changes of other players: only a real change is a new version
			if (not this->tiles or *tiles != *this->tiles or view != this->tiles_player) {
				this->tiles = std::move(tiles);
				this->tiles_version += 1;
				tiles_changed = true;
			}
			this->tiles_grid_revision = this->grid.revision();
			this->tiles_player = view;
		}
		snap->tiles = this->tiles;
	}
	else if (this->tiles) {
		this->tiles = nullptr;
		this->tiles_version += 1;
		tiles_changed = true;
	}
	snap->tiles_version = this->tiles_version;

	auto classify_entity = [&](const EntityInfo &info) {
		if (not view_fog) {
			return display_t::NORMAL;
		}
		const bool own = info.owner == view;
		const bool gaia_static = this->neutral_player(info.owner) and not info.movable;
		const bool remembered = info.building and this->remembers(view, info.id);
		return classify(own, info.movable, gaia_static, remembered,
		                this->grid.footprint_state(view, info.ne, info.se, info.radius));
	};
	auto make_dot = [&](const EntityInfo &info, display_t display) {
		MinimapDot dot;
		dot.ne = static_cast<float>(info.ne);
		dot.se = static_cast<float>(info.se);
		dot.radius = static_cast<float>(info.radius);
		dot.kind = info.kind;
		dot.player = static_cast<uint8_t>(std::min<player_id_t>(info.owner, 255));
		dot.fogged = display == display_t::FOGGED;
		dot.military = info.military;
		return dot;
	};
	auto by_paint_order = [](const MinimapDot &a, const MinimapDot &b) {
		if (a.kind != b.kind) {
			return static_cast<uint8_t>(a.kind) > static_cast<uint8_t>(b.kind);
		}
		if (a.se != b.se) {
			return a.se < b.se;
		}
		return a.ne < b.ne;
	};

	// static layer (trees, mines, buildings): rebuilt only when the tiles, the static entities or the
	// remembered buildings of the view player changed
	if (not this->static_display or static_changed or tiles_changed or view_memory_changed or view != this->static_view) {
		auto display = std::make_shared<std::unordered_map<uint32_t, uint8_t>>();
		auto dots = std::make_shared<std::vector<MinimapDot>>();
		dots->reserve(this->static_cache.size());
		for (const auto &[id, info] : this->static_cache) {
			const auto d = classify_entity(info);
			if (d != display_t::NORMAL) {
				display->emplace(static_cast<uint32_t>(id), static_cast<uint8_t>(d));
			}
			if (d != display_t::HIDDEN) {
				dots->push_back(make_dot(info, d));
			}
		}
		// stand-ins (spec 05 §5.6): foreign buildings and resources the view player saw stay "last seen"
		// in the fog after they are gone, until their place is in sight again
		if (view != this->static_view) {
			this->seen_static.clear();
		}
		if (view_fog) {
			for (const auto &[id, info] : this->static_cache) {
				if (info.owner != view
				    and this->grid.footprint_state(view, info.ne, info.se, info.radius) == tile_state_t::VISIBLE) {
					this->seen_static[id] = info;
				}
			}
			for (auto it = this->seen_static.begin(); it != this->seen_static.end();) {
				const auto &info = it->second;
				if (this->static_cache.contains(it->first)) {
					++it;
					continue;
				}
				if (this->grid.footprint_state(view, info.ne, info.se, info.radius) == tile_state_t::VISIBLE) {
					it = this->seen_static.erase(it);
					continue;
				}
				display->emplace(static_cast<uint32_t>(it->first), static_cast<uint8_t>(display_t::FOGGED));
				dots->push_back(make_dot(info, display_t::FOGGED));
				++it;
			}
		}
		else {
			this->seen_static.clear();
		}
		std::sort(dots->begin(), dots->end(), by_paint_order);
		this->static_display = std::move(display);
		this->static_dots = std::move(dots);
		this->static_view = view;
		this->static_rebuilds += 1;
	}
	snap->static_display = this->static_display;
	snap->static_dots = this->static_dots;
	// moving entities (units, animals): every tick
	snap->dots.reserve(movers.size());
	for (const auto &info : movers) {
		const auto d = classify_entity(info);
		if (d != display_t::NORMAL) {
			snap->display.emplace(static_cast<uint32_t>(info.id), static_cast<uint8_t>(d));
		}
		if (d != display_t::HIDDEN) {
			snap->dots.push_back(make_dot(info, d));
		}
	}
	std::sort(snap->dots.begin(), snap->dots.end(), by_paint_order);
	// counts for the log (every 5 s of game time): entities the view player sees darkened / not at all
	if (this->tick_count % 50 == 0) {
		this->shown_fogged = 0;
		this->shown_hidden = 0;
		const std::unordered_map<uint32_t, uint8_t> *dynamic_layer = &snap->display;
		for (const auto *layer : {dynamic_layer, snap->static_display.get()}) {
			if (layer == nullptr) {
				continue;
			}
			for (const auto &[eid, d] : *layer) {
				(d == static_cast<uint8_t>(display_t::HIDDEN) ? this->shown_hidden : this->shown_fogged) += 1;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock{this->shared_mutex};
		this->published_version += 1;
		snap->version = this->published_version;
		this->published = std::move(snap);
	}

	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	this->log_stats(ms, time);

	this->loop->create_event(this->handler,
	                         this->shared_from_this(),
	                         state,
	                         time + time::time_t::from_double(TICK),
	                         openage::event::EventHandler::param_map::map_t{});
}

void VisionState::log_stats(double ms, const time::time_t &time) {
	this->stats_ms_sum += ms;
	this->stats_ms_max = std::max(this->stats_ms_max, ms);
	this->stats_ticks += 1;
	const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (this->stats_logged_at < 0.0) {
		this->stats_logged_at = now;
	}
	if (now - this->stats_logged_at < 10.0) {
		return;
	}
	const auto view = this->view_player.load();
	log::log(INFO << "Vision: " << this->stats_ticks << " ticks, mean "
	              << this->stats_ms_sum / static_cast<double>(std::max<size_t>(this->stats_ticks, 1)) << " ms, max "
	              << this->stats_ms_max << " ms, " << this->stamps.size() << " sight circles, "
	              << this->restamps << " restamps; player " << view << ": " << this->grid.visible_count(view)
	              << " visible, " << this->grid.explored_count(view) << " explored of " << this->grid.tiles()
	              << " tiles, " << (view < this->memory.size() ? this->memory[view].size() : 0)
	              << " remembered buildings, " << this->shown_fogged << " entities in the fog, "
	              << this->shown_hidden << " hidden, static layer rebuilt " << this->static_rebuilds << " times, t="
	              << time.to_double() << " s");
	this->stats_logged_at = now;
	this->stats_ms_sum = 0.0;
	this->stats_ms_max = 0.0;
	this->stats_ticks = 0;
	this->restamps = 0;
	this->static_rebuilds = 0;
}

// ---------------------------------------------------------------- event handler

VisionEventHandler::VisionEventHandler() :
	OnceEventHandler{"game.vision_tick"} {
}

void VisionEventHandler::setup_event(const std::shared_ptr<openage::event::Event> & /* event */,
                                     const std::shared_ptr<openage::event::State> & /* state */) {
}

void VisionEventHandler::invoke(openage::event::EventLoop & /* loop */,
                                const std::shared_ptr<openage::event::EventEntity> &target,
                                const std::shared_ptr<openage::event::State> &state,
                                const time::time_t &time,
                                const param_map & /* params */) {
	auto vision = std::dynamic_pointer_cast<VisionState>(target);
	auto gstate = std::dynamic_pointer_cast<GameState>(state);
	if (vision and gstate) {
		vision->tick(gstate, time);
	}
}

time::time_t VisionEventHandler::predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> & /* target */,
                                                     const std::shared_ptr<openage::event::State> & /* state */,
                                                     const time::time_t &at) {
	return at;
}

} // namespace openage::gamestate::vision
