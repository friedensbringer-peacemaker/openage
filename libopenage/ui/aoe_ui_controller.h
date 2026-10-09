// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/hud_info.h"
#include "gamestate/map_settings.h"
#include "ui/agesxr/aoe_feed.h"
#include "ui/agesxr/xr_aoe_ui.h"
#include "ui/game_ui_controller.h"
#include "ui/ui_controller.h"


namespace openage::ui {

/**
 * Extra engine access of the AoE layout (XR fork).
 */
struct AoeHooks {
	/// camera view on the map: centre (cx, cy) and size (w, h) as fractions of
	/// the map (0..1 along the two map axes); false = unknown
	std::function<bool(float &cx, float &cy, float &w, float &h)> camera_view{};
	/// select exactly these entities (portrait of a multiple selection)
	std::function<void(const std::vector<uint64_t> &ids)> select{};
	/// save / load the game (game menu); null = not available yet
	std::function<bool()> save{};
	std::function<bool()> load{};
};

/**
 * Game user interface in the layout of Age of Empires II (XR fork,
 * docs/UI-SPEC-AOE.md stage S1): resource bar at the top, command grid,
 * selection panel and minimap placeholder in a bar at the bottom of the image,
 * message lines, context menu, game menu with settings / new map / surrender
 * and the match board. Desktop and Quest draw the same interface into the
 * engine image; the XR layer only gets UiFeedbackState (haptics, focus mode).
 *
 * Input: mouse (clicks on the bars never reach the game), hotkeys Q W E R T /
 * A S D F G / Z X C V B, Esc / F10 = game menu, F8 = stick focus into the
 * command grid, arrow keys / Return move and trigger the focus (the Quest app
 * maps X, the stick and A to them).
 */
class AoeUiController : public UiController {
public:
	/// right button held this long opens the context menu (shorter: the usual right click)
	static constexpr double HOLD_SECONDS = 0.25;
	/// poll interval of the engine data (s)
	static constexpr double POLL_SECONDS = 0.25;
	/// keys of the focus mode (Qt values)
	static constexpr int KEY_FOCUS = 0x01000037;  // F8
	static constexpr int KEY_MENU = 0x01000039;   // F10

	/**
	 * @param map Map settings of the running game.
	 * @param hooks Engine access (shared with the classic interface).
	 * @param aoe Extra engine access of this layout.
	 * @param demo Test schedule "what@seconds,...": board, menu, settings,
	 *             surrender, focus, restart (render checks without a user).
	 * @param quest Show the hint "App beenden: VR-Menü" in the game menu.
	 */
	AoeUiController(const gamestate::MapSettings &map,
	                UiHooks hooks,
	                AoeHooks aoe,
	                const std::string &demo = {},
	                bool quest = false);

	bool init() override;
	void resize(size_t width, size_t height) override;
	bool on_event(const renderer::WindowEvent &ev, double now) override;
	void update(double now) override;

	int hud_width() const override {
		return 16;
	}
	int hud_height() const override {
		return 16;
	}
	const uint32_t *hud_pixels(double /*now*/) override {
		return nullptr;
	}
	uint32_t hud_version() const override {
		return 0;
	}
	Bands hud_band_rows() const override {
		return {};
	}
	void hud_screen_rect(int &x0, int &y0, int &x1, int &y1) const override {
		x0 = y0 = x1 = y1 = 0;
	}

	const uint32_t *overlay_pixels() override;
	uint32_t overlay_version() const override;
	Bands overlay_band_rows() const override;
	int overlay_width() const override;
	int overlay_height() const override;

	bool menu_open() const override;
	bool context_open() const override;
	bool board_open() const override;
	bool user_paused() const override;
	int camera_bottom_px() const override;
	UiFeedbackState feedback() const override;

	/// the interface (layout for replays and tests)
	const agesxr::AoeUi &ui() const {
		return this->aoe_ui;
	}

private:
	void poll(double now);
	void apply_pause();
	bool open_context_menu(int x, int y);
	void context_command(int id);
	void handle(const agesxr::AoeUi::Result &result, double now);
	void open_menu(bool open, double now);
	void production_command(int code, const std::string &label);
	std::string map_info() const;
	gamestate::MapSettings menu_map_settings() const;
	void show_board(bool good, const std::string &headline, const std::vector<std::string> &lines);
	void run_demo(double now);

	gamestate::MapSettings map;
	UiHooks hooks;
	AoeHooks aoe;
	agesxr::AoeUi aoe_ui;
	agesxr::AoeFeed feed;
	size_t width = 1024;
	size_t height = 768;
	double last_poll = -1.0;
	double start_time = -1.0;
	double last_now = 0.0;
	engine::HudInfo info{};
	bool paused_sent = false;
	bool menu_pause = false;
	bool match_over = false;
	// right button hold
	bool right_held = false;
	bool right_context = false;
	double right_down_at = 0.0;
	int right_x = 0;
	int right_y = 0;
	// position of the open context menu (target of its commands)
	int context_x = 0;
	int context_y = 0;
	// left click on the interface (its release is swallowed as well)
	bool ui_click = false;
	bool left_held = false;
	// keys whose press we consumed (their release is consumed too)
	std::vector<int> swallowed_keys{};
	// redraw statistics (log every 10 s)
	double draw_log_at = 0.0;
	double draw_ms_sum = 0.0;
	double draw_ms_max = 0.0;
	long draw_rows = 0;
	long draw_count = 0;
	struct DemoStep {
		std::string what;
		double at;
		bool done = false;
	};
	std::vector<DemoStep> demo{};
};

} // namespace openage::ui
