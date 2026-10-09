// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "renderer/stages/hud/color_batches.h"
#include "util/path.h"


namespace openage::renderer {
class Renderer;
class RenderPass;
class ShaderProgram;
class Texture2d;
class Window;

namespace hud {

/**
 * Markers on the ground (XR fork): selection ellipses under units, footprint
 * diamonds of selected buildings and the footprint of the placement mode. The
 * pass lies between the terrain pass and the world pass, so the sprites cover
 * the markers like in the classic games. The presenter computes the triangles
 * in normalized device coordinates every frame.
 */
class GroundMarkerStage {
public:
	GroundMarkerStage(const std::shared_ptr<Window> &window,
	                  const std::shared_ptr<renderer::Renderer> &renderer,
	                  const util::Path &shaderdir);

	std::shared_ptr<renderer::RenderPass> get_render_pass();

	/// triangles of the next frame (render thread)
	void set_batches(std::vector<ColorBatch> &&batches);

	/// upload the batches (render thread, once per frame)
	void update();

	void resize(size_t width, size_t height);

private:
	std::shared_ptr<renderer::Renderer> renderer;
	std::shared_ptr<renderer::RenderPass> render_pass;
	std::shared_ptr<renderer::ShaderProgram> shader;
	std::shared_ptr<renderer::Texture2d> output_texture;
	std::shared_ptr<renderer::Texture2d> depth_texture;
	std::unique_ptr<ColorBatches> batches;
	std::vector<ColorBatch> pending;
	bool changed = false;
	std::mutex mutex;
};

} // namespace hud
} // namespace openage::renderer
