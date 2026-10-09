// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/hud_info.h"
#include "gamestate/map_settings.h"
#include "gamestate/save_format.h"
#include "renderer/window_events.h"
#include "ui/agesxr/hud_feed.h"
#include "ui/agesxr/xr_game_ui.h"
#include "ui/agesxr/xr_hud.h"

namespace openage {

namespace gamestate::prod {
class Production;
}

namespace time {
class Clock;
}

namespace ui {

/**
 * Command of the context menu (XR fork), sent like a right click at a screen pixel.
 */
enum class ui_command_t {
	/// walk to the point, ignore what is under the cursor
	MOVE_PLAIN,
	/// like a right click: enemy, foundation, resource or ground (buildings: rally point)
	MOVE,
	/// gather the resource under the cursor (others walk there)
	GATHER,
	/// attack the enemy under the cursor (nothing without one)
	ATTACK,
};

/**
 * What the game user interface needs from the engine (XR fork). All callbacks are
 * invoked from the presenter thread.
 */
struct UiHooks {
	/// HUD snapshot of the controlled player (Engine::query_hud)
	std::function<engine::HudInfo()> query_hud{};
	/// production interface (train, build, cancel), may be null
	std::shared_ptr<gamestate::prod::Production> production{};
	/// simulation clock (pause, speed, game time)
	std::shared_ptr<time::Clock> clock{};
	/// send a command of the context menu for the selection at a window pixel
	std::function<void(ui_command_t type, int x, int y)> send_command{};
	/// restart the engine with other map settings (game menu "Neue Karte")
	std::function<void(const gamestate::MapSettings &settings)> restart{};
	/// quit the game (game menu "Beenden", confirmed)
	std::function<void()> quit{};
	/// save games (gamestate/save_format.h): save into a slot, load a slot (restart), list the slots
	std::function<bool(int slot)> save_slot{};
	std::function<std::string(int slot)> load_slot{};
	std::function<std::vector<gamestate::save::SlotInfo>()> list_slots{};
	/// camera to an entity and select it (orders bar)
	std::function<void(uint64_t id)> focus_entity{};
};

/**
 * Game user interface drawn into the engine image (XR fork): the HUD bar of the
 * XR layer (resources, population, game time, selection, production buttons,
 * status), a context menu for the selection (right button held, Alt + right
 * click or middle click), the game menu (Esc / F10: resume, pause, speed, new
 * map, quit) and the match board (victory / defeat).
 *
 * GL-free and engine-light: the window events of the presenter come in through
 * on_event(), the pixels go out through hud_*() and overlay_*() to
 * renderer::stages::ui::UiRenderStage, which uploads them. The same CPU canvas
 * code draws the VR HUD and VR menu of the Quest app (ui/agesxr, copied from the
 * superproject). The Quest app keeps this interface off (window_settings::ui).
 */
class GameUiController {
public:
	/// right button held this long opens the context menu (shorter: the usual right click)
	static constexpr double HOLD_SECONDS = 0.25;
	/// poll interval of the engine data (s)
	static constexpr double POLL_SECONDS = 0.25;

	/**
	 * @param map Map settings of the running game (for the game menu defaults).
	 * @param hooks Engine access.
	 * @param demo Test schedule "what@seconds,...": "board@12" shows a demo match
	 *             board after 12 s, "restart@8" requests a restart with the next
	 *             map number after 8 s (render checks without a user).
	 */
	GameUiController(const gamestate::MapSettings &map, UiHooks hooks, const std::string &demo = {});

	/// load the fonts; false = no system font (the layout still works)
	bool init();
	void resize(size_t width, size_t height);

	/**
	 * Handle a window event before the input manager.
	 *
	 * @return true if the event belongs to the interface (not passed to the game).
	 */
	bool on_event(const renderer::WindowEvent &ev, double now);

	/// once per frame: poll the engine data, hold timer, pause state
	void update(double now);

	// ---- output for the render stage
	const uint32_t *hud_pixels(double now);
	uint32_t hud_version() const;
	const std::vector<agesxr::VrHud::DirtyBand> &hud_bands() const;
	static constexpr int hud_texture_width() { return agesxr::VrHud::kWidth; }
	static constexpr int hud_texture_height() { return agesxr::VrHud::kHeight; }
	/// HUD bar on screen (window pixels, origin top left): full width, height scaled
	void hud_screen_rect(int &x0, int &y0, int &x1, int &y1) const;

	const uint32_t *overlay_pixels();
	uint32_t overlay_version() const;
	int overlay_dirty_y0() const;
	int overlay_dirty_y1() const;
	int overlay_width() const;
	int overlay_height() const;

	// ---- state (tests, logs)
	bool menu_open() const;
	bool context_open() const;
	bool board_open() const;
	bool user_paused() const;
	const agesxr::HudModel &hud_model() const;

private:
	void poll(double now);
	void apply_pause();
	bool over_hud(double x, double y) const;
	void hud_coords(double x, double y, float &hx, float &hy) const;
	bool open_context_menu(int x, int y);
	void context_command(int id);
	void menu_action(agesxr::GameUi::Result result);
	void open_menu(bool open);
	void production_command(int code, const std::string &label);
	void refresh_slots();
	void save_slot(int slot, const char *why);
	void load_slot(int slot, const char *why);
	std::string map_info() const;
	gamestate::MapSettings menu_map_settings() const;
	void run_demo(double now);

	gamestate::MapSettings map;
	UiHooks hooks;
	agesxr::VrHud hud;
	agesxr::HudFeed feed;
	agesxr::GameUi ui;
	size_t width = 1024;
	size_t height = 768;
	double last_poll = -1.0;
	double start_time = -1.0;
	engine::HudInfo info{};
	bool paused_sent = false;
	bool menu_pause = false;
	// right button hold
	bool right_held = false;
	bool right_context = false;
	double right_down_at = 0.0;
	int right_x = 0;
	int right_y = 0;
	// position of the open context menu (target of its commands)
	int context_x = 0;
	int context_y = 0;
	// left click on a HUD button (its release is swallowed as well)
	bool hud_click = false;
	bool left_held = false;
	// keys whose press we consumed (their release is consumed too)
	std::vector<int> swallowed_keys{};
	// autosave every AUTOSAVE_SECONDS of game time
	gamestate::save::AutosaveTimer autosave{};
	// demo schedule
	struct DemoStep {
		std::string what;
		double at;
		bool done = false;
	};
	std::vector<DemoStep> demo{};
};

} // namespace ui
} // namespace openage
