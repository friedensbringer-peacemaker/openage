// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "event/evententity.h"
#include "event/eventhandler.h"
#include "gamestate/map_settings.h"
#include "gamestate/types.h"
#include "gamestate/vision/vision_grid.h"
#include "gamestate/vision/vision_snapshot.h"
#include "time/time.h"


namespace openage {

namespace event {
class EventLoop;
} // namespace event

namespace gamestate {
class GameState;

namespace vision {

/**
 * Fog of war of the XR fork: line of sight of every player, explored tiles,
 * remembered foreign buildings, and what the local player is shown.
 *
 * Runs in the simulation thread as a periodic event ("game.vision_tick", every
 * TICK seconds of simulation time). Per tick: every own unit or building of a
 * player stamps a sight circle of its nyan LineOfSight range (combat stats, so
 * upgrades of the range are picked up) into the grid of its owner; only
 * entities that moved to another tile, changed owner or range restamp. Then
 * foreign buildings in sight are remembered, the display of every entity for
 * the view player is classified (classify()) and a Snapshot is published for
 * the renderer and the minimap.
 *
 * Queries for the simulation (combat, ai, commands) answer "visible" for
 * everything while the vision has not started or the mode is ALL, so host
 * checks without a vision behave as before.
 */
class VisionState : public openage::event::EventEntity
	, public std::enable_shared_from_this<VisionState> {
public:
	/// period of the vision tick (simulation seconds)
	static constexpr double TICK = 0.1;

	explicit VisionState(const std::shared_ptr<openage::event::EventLoop> &loop);
	~VisionState() = default;

	size_t id() const override;
	std::string idstr() const override;

	/**
	 * Create the grids for the map of \p state and start the ticks. The first tick
	 * runs at once, so the start area is visible before the first frame.
	 *
	 * @param state Game state with map and players.
	 * @param mode Vision mode of the game.
	 * @param neutral Players without sight (gaia).
	 * @param time Start time.
	 */
	void start(const std::shared_ptr<GameState> &state,
	           vision_mode_t mode,
	           const std::unordered_set<player_id_t> &neutral,
	           const time::time_t &time);

	/// minimap ground colours (index ne + se * ne_size), static for the game
	void set_terrain_colors(std::vector<uint32_t> &&colors);

	/// player whose view is published (thread-safe; default 0)
	void set_view_player(player_id_t player);

	/// event handler: one tick, schedules the next one
	void tick(const std::shared_ptr<GameState> &state, const time::time_t &time);

	// ---- queries, simulation thread

	bool is_started() const;
	vision_mode_t get_mode() const;

	/**
	 * \p player sees a footprint (any tile visible). True for everything while
	 * the vision has not started or in mode ALL, and for neutral players.
	 */
	bool sees(player_id_t player, double ne, double se, double radius) const;

	/// best tile state of a footprint for \p player (VISIBLE if the vision is off)
	tile_state_t footprint_state(player_id_t player, double ne, double se, double radius) const;

	/// foreign building \p id was seen by \p player (or given with remember())
	bool remembers(player_id_t player, entity_id_t id) const;

	/**
	 * Let \p player know a foreign building (computer opponents know the start
	 * positions of their enemies; the building counts as seen).
	 */
	void remember(player_id_t player, entity_id_t id, double ne, double se, double radius);

	/**
	 * May \p player target or pick entity \p id: visible, or a remembered building in the fog.
	 */
	bool knows(player_id_t player, entity_id_t id, double ne, double se, double radius, bool building) const;

	/// explored tiles of a player as text (save games, VisionGrid::explored_text)
	std::string explored_text(player_id_t player) const;
	/// restore the explored tiles of a player (save games), false if the text does not fit
	bool set_explored_text(player_id_t player, const std::string &text);

	/// the grid (tests, simulation thread)
	const VisionGrid &get_grid() const;

	// ---- thread-safe

	/// latest published view (nullptr before the first tick)
	std::shared_ptr<const Snapshot> snapshot() const;

private:
	struct Stamp {
		player_id_t player = 0;
		int ne = 0;
		int se = 0;
		int q = 0;
	};
	/// what the tick needs of one entity
	struct EntityInfo {
		entity_id_t id = 0;
		player_id_t owner = 0;
		double ne = 0.0;
		double se = 0.0;
		/// footprint radius for the visibility of the entity (0 for units: the tile of the unit)
		double radius = 0.0;
		bool movable = false;
		bool building = false;
		bool military = false;
		dot_kind_t kind = dot_kind_t::OTHER;
	};
	struct Place {
		double ne = 0.0;
		double se = 0.0;
		double radius = 0.0;
	};

	bool neutral_player(player_id_t player) const;
	void publish(const std::shared_ptr<GameState> &state, const time::time_t &time);
	void log_stats(double ms, const time::time_t &time);

	std::shared_ptr<openage::event::EventLoop> loop;
	std::shared_ptr<openage::event::EventHandler> handler;

	bool started = false;
	vision_mode_t mode = vision_mode_t::ALL;
	std::unordered_set<player_id_t> neutral;
	/// first player id without a grid (gaia and above)
	size_t grid_players = 0;
	VisionGrid grid;
	/// sight circle per entity that has one
	std::unordered_map<entity_id_t, Stamp> stamps;
	/// remembered foreign buildings per player (ordered: deterministic)
	std::vector<std::map<entity_id_t, Place>> memory;
	/// entities that cannot move (trees, mines, buildings): read once, refreshed once a second
	std::unordered_map<entity_id_t, EntityInfo> static_cache;
	uint64_t tick_count = 0;
	/// static entities of the last tick (count, id sum): a change rebuilds the static layer
	size_t static_seen = 0;
	uint64_t static_sum = 0;
	/// static layer of the view (trees, mines, buildings), shared by the snapshots until it changes
	std::shared_ptr<const std::unordered_map<uint32_t, uint8_t>> static_display;
	std::shared_ptr<const std::vector<MinimapDot>> static_dots;
	player_id_t static_view = UINT64_MAX;
	size_t static_rebuilds = 0;
	/// entities of the last snapshot shown darkened / hidden (log)
	size_t shown_fogged = 0;
	size_t shown_hidden = 0;

	/// last published tile states of the view player
	std::shared_ptr<const std::vector<uint8_t>> tiles;
	uint64_t tiles_grid_revision = UINT64_MAX;
	uint64_t tiles_version = 0;
	player_id_t tiles_player = UINT64_MAX;
	std::shared_ptr<const std::vector<uint32_t>> terrain;

	// statistics (log every 10 s of real time)
	double stats_ms_sum = 0.0;
	double stats_ms_max = 0.0;
	size_t stats_ticks = 0;
	size_t stats_restamps = 0;
	double stats_logged_at = -1.0;
	size_t restamps = 0;

	// ---- shared with other threads
	std::atomic<player_id_t> view_player{0};
	mutable std::mutex shared_mutex;
	std::shared_ptr<const Snapshot> published;
	uint64_t published_version = 0;
};

/**
 * Event handler of the vision ticks.
 */
class VisionEventHandler : public openage::event::OnceEventHandler {
public:
	VisionEventHandler();
	~VisionEventHandler() = default;

	void setup_event(const std::shared_ptr<openage::event::Event> &event,
	                 const std::shared_ptr<openage::event::State> &state) override;

	void invoke(openage::event::EventLoop &loop,
	            const std::shared_ptr<openage::event::EventEntity> &target,
	            const std::shared_ptr<openage::event::State> &state,
	            const time::time_t &time,
	            const param_map &params) override;

	time::time_t predict_invoke_time(const std::shared_ptr<openage::event::EventEntity> &target,
	                                 const std::shared_ptr<openage::event::State> &state,
	                                 const time::time_t &at) override;
};

} // namespace vision
} // namespace gamestate
} // namespace openage
