// Copyright 2023-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>

#include <eigen3/Eigen/Dense>

#include "util/path.h"

namespace openage {

namespace time {
class Clock;
}

namespace renderer {
class Geometry;
class Renderer;
class RenderPass;
class UniformInput;
class ShaderProgram;
class Texture2d;
class Window;

namespace camera {
class Camera;
}

namespace resources {
class AssetManager;
}

namespace hud {
class HudDragObject;
class DragRenderEntity;

/**
 * Frame around a selected game entity (XR fork).
 */
struct SelectionMarker {
	/// screen rectangle of the sprite (left, bottom, right, top) in normalized device coordinates
	Eigen::Vector4f ndc;
	/// owned by the controlled player (white), else only displayed (yellow)
	bool own;
};

/**
 * Renderer for the "Heads-Up Display" (HUD).
 * Draws UI elements that are not part of the GUI, e.g. health bars, selection boxes, minimap, etc.
 *
 * TODO: Currently only supports drag selection.
 */
class HudRenderStage {
public:
	/**
	 * Create a new render stage for the HUD.
	 *
	 * @param window openage window targeted for rendering.
	 * @param renderer openage low-level renderer.
	 * @param camera Camera used for the rendered scene.
	 * @param shaderdir Directory containing the shader source files.
	 * @param asset_manager Asset manager for loading resources.
	 * @param clock Simulation clock for timing animations.
	 */
	HudRenderStage(const std::shared_ptr<Window> &window,
	               const std::shared_ptr<renderer::Renderer> &renderer,
	               const std::shared_ptr<renderer::camera::Camera> &camera,
	               const util::Path &shaderdir,
	               const std::shared_ptr<renderer::resources::AssetManager> &asset_manager,
	               const std::shared_ptr<time::Clock> clock);
	~HudRenderStage() = default;

	/**
	 * Get the render pass of the HUD renderer.
	 *
	 * @return Render pass for HUD drawing.
	 */
	std::shared_ptr<renderer::RenderPass> get_render_pass();

	/**
	 * Add a new render entity for drag selection.
	 *
	 * @param render_entity New render entity.
	 */
	void add_drag_entity(const std::shared_ptr<DragRenderEntity> entity);

	/**
	 * Remove the render object for drag selection.
	 *
	 * @param render_entity Render entity to remove.
	 */
	void remove_drag_entity();

	/**
	 * Frames around the selected game entities (XR fork), drawn until the
	 * next call. Call in the render thread, e.g. once per frame.
	 *
	 * @param markers Screen rectangles of the selection.
	 */
	void set_selection_markers(std::vector<SelectionMarker> &&markers);

	/**
	 * Rally point flag of the selected building (XR fork): foot of the flag in
	 * normalized device coordinates, nothing = no flag. Drawn with the own
	 * selection frames (white).
	 */
	void set_rally_flag(std::optional<Eigen::Vector2f> ndc);

	/**
	 * Update the render entities and render positions.
	 */
	void update();

	/**
	 * Resize the FBO for the HUD rendering. This basically updates the output
	 * texture size.
	 *
	 * @param width New width of the FBO.
	 * @param height New height of the FBO.
	 */
	void resize(size_t width, size_t height);

private:
	/**
	 * Create the render pass for HUD drawing.
	 *
	 * Called during initialization of the HUD renderer.
	 *
	 * @param width Width of the FBO.
	 * @param height Height of the FBO.
	 * @param shaderdir Directory containing the shader source files.
	 */
	void initialize_render_pass(size_t width,
	                            size_t height,
	                            const util::Path &shaderdir);

	/**
	 * Reference to the openage renderer.
	 */
	std::shared_ptr<renderer::Renderer> renderer;

	/**
	 * Camera for model uniforms.
	 */
	std::shared_ptr<renderer::camera::Camera> camera;

	/**
	 * Texture manager for loading assets.
	 */
	std::shared_ptr<renderer::resources::AssetManager> asset_manager;

	/**
	 * Render pass for the HUD drawing.
	 */
	std::shared_ptr<renderer::RenderPass> render_pass;

	/**
	 * Render object for the drag select rectangle.
	 */
	std::shared_ptr<HudDragObject> drag_object;

	/**
	 * Shader for rendering the drag select rectangle.
	 */
	std::shared_ptr<renderer::ShaderProgram> drag_select_shader;

	/**
	 * Selection frames (XR fork): rectangles and, per color group (own, other),
	 * the geometry + uniforms in the render pass.
	 */
	std::vector<SelectionMarker> selection_markers;
	std::optional<Eigen::Vector2f> rally_flag{};
	std::array<std::shared_ptr<renderer::Geometry>, 2> marker_geometry;
	std::array<std::shared_ptr<renderer::UniformInput>, 2> marker_uniforms;
	std::array<size_t, 2> marker_vertices{0, 0};

	/**
	 * Draw the selection frames (XR fork), part of update().
	 */
	void update_selection_markers();

	/**
	 * Simulation clock for timing animations.
	 */
	std::shared_ptr<time::Clock> clock;

	/**
	 * Output texture.
	 */
	std::shared_ptr<renderer::Texture2d> output_texture;

	/**
	 * Depth texture.
	 */
	std::shared_ptr<renderer::Texture2d> depth_texture;

	/**
	 * Mutex for protecting threaded access.
	 */
	std::shared_mutex mutex;
};
} // namespace hud
} // namespace renderer
} // namespace openage
