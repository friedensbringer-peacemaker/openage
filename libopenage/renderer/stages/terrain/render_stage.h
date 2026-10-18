// Copyright 2022-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <vector>

#include <eigen3/Eigen/Dense>

#include "coord/scene.h"
#include "util/path.h"
#include "util/vector.h"


namespace openage {

namespace time {
class Clock;
}

namespace renderer {
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

namespace terrain {
class RenderEntity;
class TerrainRenderMesh;
class TerrainRenderModel;

/**
 * Manage and render terrain geometry and graphics.
 */
class TerrainRenderStage {
public:
	/**
	 * Create a new render stage for the terrain.
	 *
	 * @param window openage window targeted for rendering.
	 * @param renderer openage low-level renderer.
	 * @param camera Camera used for the rendered scene.
	 * @param shaderdir Directory containing the shader source files.
	 * @param asset_manager Asset manager for loading resources.
	 * @param clock Simulation clock for timing animations.
	 */
	TerrainRenderStage(const std::shared_ptr<Window> &window,
	                   const std::shared_ptr<renderer::Renderer> &renderer,
	                   const std::shared_ptr<renderer::camera::Camera> &camera,
	                   const util::Path &shaderdir,
	                   const std::shared_ptr<renderer::resources::AssetManager> &asset_manager,
	                   const std::shared_ptr<time::Clock> &clock);
	~TerrainRenderStage() = default;

	/**
	 * Get the render pass of the terrain renderer.
	 *
	 * @return Render pass for terrain drawing.
	 */
	std::shared_ptr<renderer::RenderPass> get_render_pass();

	/**
	 * Add a new render entity to the terrain renderer.
	 *
	 * This creates a new terrain chunk and add it to the model.
	 *
	 * @param render_entity New render entity.
	 */
	void add_render_entity(const std::shared_ptr<RenderEntity> entity,
	                       const util::Vector2s chunk_size,
	                       const coord::scene2_delta chunk_offset);

	/**
	 * Update the terrain mesh and texture information.
	 */
	void update();

	/**
	 * Resize the FBO for the terrain rendering. This basically updates the output
	 * texture size.
	 *
	 * @param width New width of the FBO.
	 * @param height New height of the FBO.
	 */
	void resize(size_t width, size_t height);

	/**
	 * Fog of war (XR fork, gamestate/vision): darken explored tiles, black for
	 * unexplored ones, soft edges (the shader blends the tile states). Call in
	 * the render thread; only a new version uploads the texture.
	 *
	 * @param tiles Tile states (0 unexplored, 1 explored, 2 visible), index
	 *              ne + se * ne_size; nullptr = everything visible (no fog pass).
	 * @param ne_size Tiles along ne.
	 * @param se_size Tiles along se.
	 * @param version Changes whenever the tile states change.
	 */
	void set_fog(const std::vector<uint8_t> *tiles, size_t ne_size, size_t se_size, uint64_t version);

private:
	/**
	 * Create the render pass for terrains.
	 *
	 * Called during initialization of the terrain renderer.
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
	 * Camera for view and projection matrices.
	 */
	std::shared_ptr<renderer::camera::Camera> camera;

	/**
	 * Engine interface for updating terrain draw information.
	 */
	std::shared_ptr<RenderEntity> render_entity;

	/**
	 * 3D model of the terrain.
	 */
	std::shared_ptr<TerrainRenderModel> model;

	/**
	 * Render pass for the terrain drawing.
	 */
	std::shared_ptr<renderer::RenderPass> render_pass;

	/**
	 * Shader for rendering the world objects.
	 */
	std::shared_ptr<renderer::ShaderProgram> display_shader;

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
	 * Fog of war (XR fork): one texel per tile, parameters (1 / ne_size, 1 / se_size, on).
	 */
	std::shared_ptr<renderer::Texture2d> fog_texture;
	Eigen::Vector3f fog_params{1.0f, 1.0f, 0.0f};
	size_t fog_ne = 0;
	size_t fog_se = 0;
	uint64_t fog_version = 0;
	/// fog uniforms of all meshes must be set again (new texture or parameters)
	bool fog_dirty = true;

	/**
	 * Mutex for protecting threaded access.
	 */
	std::shared_mutex mutex;
};

} // namespace terrain
} // namespace renderer
} // namespace openage
