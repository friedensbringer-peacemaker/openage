// Copyright 2022-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
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
class ShaderProgram;
class Texture2d;
class Window;

namespace camera {
class Camera;
}

namespace resources {
class AssetManager;
}

namespace world {
class RenderEntity;
class WorldObject;

/**
 * Screen rectangle of a drawn game entity (XR fork, selection markers).
 */
struct ScreenBox {
	/// game entity
	uint32_t id;
	/// owning player
	uint32_t player;
	/// (left, bottom, right, top) in normalized device coordinates
	Eigen::Vector4f ndc;
};

/**
 * Renderer for drawing and displaying entities in the game world (units, buildings, etc.)
 */
class WorldRenderStage {
public:
	/**
	 * Enable or disable frustum culling (default = false).
	 */
	static bool ENABLE_FRUSTUM_CULLING;

	/**
	 * Create a new render stage for the game world.
	 *
	 * @param window openage window targeted for rendering.
	 * @param renderer openage low-level renderer.
	 * @param camera Camera used for the rendered scene.
	 * @param shaderdir Directory containing the shader source files.
	 * @param asset_manager Asset manager for loading resources.
	 * @param clock Simulation clock for timing animations.
	 */
	WorldRenderStage(const std::shared_ptr<Window> &window,
	                 const std::shared_ptr<renderer::Renderer> &renderer,
	                 const std::shared_ptr<renderer::camera::Camera> &camera,
	                 const util::Path &shaderdir,
	                 const std::shared_ptr<renderer::resources::AssetManager> &asset_manager,
	                 const std::shared_ptr<time::Clock> clock);
	~WorldRenderStage() = default;

	/**
	 * Get the render pass of the world renderer.
	 *
	 * @return Render pass for world drawing.
	 */
	std::shared_ptr<renderer::RenderPass> get_render_pass();

	/**
	 * Add a new render entity of the world renderer.
	 *
	 * @param render_entity New render entity.
	 */
	void add_render_entity(const std::shared_ptr<RenderEntity> entity);

	/**
	 * Update the render entities and render positions.
	 */
	void update();

	/**
	 * Resize the FBO for the world rendering. This basically updates the output
	 * texture size.
	 *
	 * @param width New width of the FBO.
	 * @param height New height of the FBO.
	 */
	void resize(size_t width, size_t height);

	/**
	 * Game entity drawn at a point of the last frame (XR fork): reads the
	 * object id texture of the world pass (one texel). Call in the render
	 * thread.
	 *
	 * @param ndc_x Horizontal position in normalized device coordinates.
	 * @param ndc_y Vertical position in normalized device coordinates.
	 *
	 * @return ID of the topmost game entity at the point, nothing on terrain.
	 */
	std::optional<uint32_t> pick(float ndc_x, float ndc_y);

	/**
	 * Screen rectangles of game entities (XR fork, selection markers).
	 *
	 * @param ids Game entities.
	 *
	 * @return Rectangles of those entities that are drawn.
	 */
	std::vector<ScreenBox> get_screen_boxes(const std::vector<uint32_t> &ids);

private:
	/**
	 * Create the render pass for world drawing.
	 *
	 * Called during initialization of the world renderer.
	 *
	 * @param width Width of the FBO.
	 * @param height Height of the FBO.
	 * @param shaderdir Directory containing the shader source files.
	 */
	void initialize_render_pass(size_t width, size_t height, const util::Path &shaderdir);

	/**
	 * Fetch the uniform IDs for the uniforms of the world shader from OpenGL
	 * and assign them to the WorldObject class.
	 *
	 * This method must be called after the shader program has been created but
	 * before any uniforms are set.
	 */
	void init_uniform_ids();

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
	 * Render pass for the world drawing.
	 */
	std::shared_ptr<renderer::RenderPass> render_pass;

	/**
	 * Render entities requested by the game world.
	 */
	std::vector<std::shared_ptr<WorldObject>> render_objects;

	/**
	 * Shader for rendering the world objects.
	 */
	std::shared_ptr<renderer::ShaderProgram> display_shader;

	/**
	 * Simulation clock for timing animations.
	 */
	std::shared_ptr<time::Clock> clock;

	/**
	 * Default geometry for every world object.
	 *
	 * Since all world objects are sprites, their mesh is always quad
	 * with the same vertex info. Reusing the geometry allows us to
	 * use the same vetrex buffer for every object.
	 */
	const std::shared_ptr<renderer::Geometry> default_geometry;

	/**
	 * Output texture.
	 */
	std::shared_ptr<renderer::Texture2d> output_texture;

	/**
	 * Depth texture.
	 */
	std::shared_ptr<renderer::Texture2d> depth_texture;

	/**
	 * ID texture.
	 */
	std::shared_ptr<renderer::Texture2d> id_texture;

	/**
	 * Mutex for protecting threaded access.
	 */
	std::shared_mutex mutex;
};
} // namespace world
} // namespace renderer
} // namespace openage
