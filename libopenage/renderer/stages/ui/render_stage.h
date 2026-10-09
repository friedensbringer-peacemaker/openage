// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <memory>

#include "util/path.h"

namespace openage {

namespace ui {
class UiController;
}

namespace renderer {
class Geometry;
class Renderer;
class RenderPass;
class ShaderProgram;
class Texture2d;
class UniformInput;
class Window;

namespace ui {

/**
 * Render stage of the game user interface (XR fork): draws the CPU canvases of
 * a ui::UiController (classic: HUD bar at the top + overlay with menus; AoE
 * layout: one window-sized overlay) as two textured quads into its own render
 * pass, which the screen stage composes after the HUD pass. Textures are
 * uploaded only when the controller reports a new version, and only the
 * changed row bands.
 */
class UiRenderStage {
public:
	UiRenderStage(const std::shared_ptr<Window> &window,
	              const std::shared_ptr<renderer::Renderer> &renderer,
	              const util::Path &shaderdir,
	              const std::shared_ptr<openage::ui::UiController> &controller);
	~UiRenderStage() = default;

	std::shared_ptr<renderer::RenderPass> get_render_pass();

	/**
	 * Update the interface (engine data, timers) and upload changed pixels.
	 *
	 * @param now Wall clock seconds (steady).
	 */
	void update(double now);

	/**
	 * Resize the pass target and the overlay texture.
	 */
	void resize(size_t width, size_t height);

private:
	void initialize_render_pass(size_t width, size_t height, const util::Path &shaderdir);
	void create_overlay_texture(size_t width, size_t height);
	/// upload rows [y0, y1) of a canvas (row 0 = top) into a texture of the same width
	void upload_rows(const std::shared_ptr<Texture2d> &texture,
	                 const uint32_t *pixels,
	                 int width,
	                 int y0,
	                 int y1);
	void update_rects();

	std::shared_ptr<renderer::Renderer> renderer;
	std::shared_ptr<openage::ui::UiController> controller;
	std::shared_ptr<renderer::RenderPass> render_pass;
	std::shared_ptr<renderer::ShaderProgram> shader;
	std::shared_ptr<renderer::Geometry> quad;
	std::shared_ptr<renderer::Texture2d> hud_texture;
	std::shared_ptr<renderer::Texture2d> overlay_texture;
	std::shared_ptr<renderer::UniformInput> hud_uniforms;
	std::shared_ptr<renderer::UniformInput> overlay_uniforms;
	std::shared_ptr<renderer::Texture2d> output_texture;
	uint32_t hud_shown = 0;
	uint32_t overlay_shown = 0;
	size_t width = 0;
	size_t height = 0;
};

} // namespace ui
} // namespace renderer
} // namespace openage
