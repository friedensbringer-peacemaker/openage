// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "renderer/window_events.h"


namespace openage::ui {

/**
 * Feedback of the game interface for an embedder (XR fork): haptics on clicks
 * (click_seq changes, never on hover), double pulse when "surrender" is armed,
 * stick focus mode and open menus (the XR layer moves the focus with the
 * stick instead of the camera).
 */
struct UiFeedbackState {
	int hover_id = 0;
	int clicked_id = 0;
	uint32_t click_seq = 0;
	bool armed = false;
	bool focus_mode = false;
	bool menu_open = false;
};

/**
 * Game user interface drawn into the engine image (XR fork). Two styles share
 * the render stage (renderer/stages/ui): the classic HUD bar
 * (GameUiController) and the AoE layout (AoeUiController, default).
 *
 * The render stage draws two CPU canvases: an optional fixed-size HUD texture
 * scaled into hud_screen_rect() and a window-sized overlay. Rows are listed as
 * [y0, y1) bands (row 0 = top) for partial uploads.
 */
class UiController {
public:
	using Bands = std::vector<std::pair<int, int>>;

	virtual ~UiController() = default;

	/// load the fonts; false = no system font (the layout still works)
	virtual bool init() = 0;
	virtual void resize(size_t width, size_t height) = 0;

	/**
	 * Handle a window event before the input manager.
	 *
	 * @return true if the event belongs to the interface (not passed to the game).
	 */
	virtual bool on_event(const renderer::WindowEvent &ev, double now) = 0;

	/// once per frame: poll the engine data, timers, pause state
	virtual void update(double now) = 0;

	// ---- HUD texture (fixed size, may be unused: size 16 x 16, no pixels)
	virtual int hud_width() const = 0;
	virtual int hud_height() const = 0;
	virtual const uint32_t *hud_pixels(double now) = 0;
	virtual uint32_t hud_version() const = 0;
	virtual Bands hud_band_rows() const = 0;
	/// HUD on screen (window pixels, origin top left); empty = not drawn
	virtual void hud_screen_rect(int &x0, int &y0, int &x1, int &y1) const = 0;

	// ---- overlay (window size)
	virtual const uint32_t *overlay_pixels() = 0;
	virtual uint32_t overlay_version() const = 0;
	virtual Bands overlay_band_rows() const = 0;
	virtual int overlay_width() const = 0;
	virtual int overlay_height() const = 0;

	// ---- state (tests, logs, embedder)
	virtual bool menu_open() const = 0;
	virtual bool context_open() const = 0;
	virtual bool board_open() const = 0;
	virtual bool user_paused() const = 0;

	/// pixels at the bottom covered by the interface (camera limit, AoE layout)
	virtual int camera_bottom_px() const {
		return 0;
	}

	virtual UiFeedbackState feedback() const {
		return {};
	}
};

} // namespace openage::ui
