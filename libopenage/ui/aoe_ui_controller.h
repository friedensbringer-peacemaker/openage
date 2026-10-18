// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/hud_info.h"
#include "gamestate/map_settings.h"
#include "gamestate/save_format.h"
#include "gamestate/vision/vision_snapshot.h"
#include "ui/agesxr/aoe_feed.h"
#include "ui/agesxr/xr_aoe_ui.h"
#include "ui/agesxr/xr_minimap.h"
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
	/// minimap (XR fork, gamestate/vision): what the controlled player sees, nullptr before the first tick
	std::function<std::shared_ptr<const gamestate::vision::Snapshot>()> minimap{};
	/// jump the camera to a point given as map fractions (u along se, v = 1 - ne fraction, as camera_view)
	std::function<void(float u, float v)> minimap_jump{};
	/// right click on the minimap: the selection moves there (attacks an enemy, gathers a resource there)
	std::function<void(float u, float v)> minimap_command{};
	// save games and the orders use UiHooks (save_slot, load_slot, list_slots, focus_entity)
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
	const uint32_t *minimap_pixels(int &x0, int &y0, int &w, int &h) const override;
	uint32_t minimap_version() const override;

	/// minimap image refresh (s): with the engine poll, 4 times a second
	static constexpr double MINIMAP_SECONDS = POLL_SECONDS;

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
	void refresh_slots();
	void save_slot(int slot, const char *why);
	void load_slot(int slot, const char *why);
	void run_demo(double now);
	void update_minimap(double now);

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
	// autosave every AUTOSAVE_SECONDS of game time (as the classic interface)
	gamestate::save::AutosaveTimer autosave{};
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
	// minimap (XR fork, gamestate/vision): image under the bar, drag, signal
	bool minimap_live = false;
	bool minimap_drag = false;
	double minimap_drag_at = 0.0;
	bool minimap_force = false;
	int minimap_mode = agesxr::kMinimapNormal;
	double minimap_at = -1.0;
	uint64_t minimap_snapshot = 0;
	std::vector<uint32_t> minimap_image{};
	std::vector<agesxr::MinimapDot> minimap_dots{};
	// ground x fog layer, kept while the tile states do not change
	std::vector<uint32_t> minimap_base{};
	uint64_t minimap_base_tiles = 0;
	const void *minimap_base_terrain = nullptr;
	const void *minimap_base_static = nullptr;
	int minimap_base_w = 0;
	int minimap_base_h = 0;
	long minimap_base_count = 0;
	int minimap_x0 = 0;
	int minimap_y0 = 0;
	int minimap_w = 0;
	int minimap_h = 0;
	uint32_t minimap_seq = 0;
	float ping_u = 0.0f;
	float ping_v = 0.0f;
	double ping_at = -1.0;
	// raster statistics (log every 10 s)
	double minimap_ms_sum = 0.0;
	double minimap_ms_max = 0.0;
	long minimap_count = 0;
	double minimap_log_at = 0.0;
};

} // namespace openage::ui
