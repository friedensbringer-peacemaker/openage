// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <vector>

#include <eigen3/Eigen/Dense>


namespace openage::renderer {
class Geometry;
class Renderer;
class RenderPass;
class ShaderProgram;
class UniformInput;

namespace hud {

/**
 * Triangles of one flat color in normalized device coordinates (XR fork):
 * (x, y) pairs, three vertices per triangle.
 */
struct ColorBatch {
	Eigen::Vector4f color{1.0f, 1.0f, 1.0f, 1.0f};
	std::vector<float> verts{};

	/// add a triangle
	void tri(float x0, float y0, float x1, float y1, float x2, float y2);
	/// add an axis-aligned rectangle (left, bottom, right, top)
	void rect(float l, float b, float r, float t);
	/// add a quadrilateral (corners in order)
	void quad(const Eigen::Vector2f &a, const Eigen::Vector2f &b, const Eigen::Vector2f &c, const Eigen::Vector2f &d);
	/// add a closed polyline of width \p w (NDC) through \p points
	void ring(const std::vector<Eigen::Vector2f> &points, float wx, float wy);
};

/**
 * Renderables of colored triangle batches in a render pass (XR fork): one
 * geometry per color slot, updated in place while its vertex count stays the
 * same. Used by the HUD stage (health bars) and the ground marker stage
 * (selection ellipses, footprints).
 */
class ColorBatches {
public:
	ColorBatches(const std::shared_ptr<Renderer> &renderer, const std::shared_ptr<ShaderProgram> &shader);

	/**
	 * Show exactly these batches (colors are matched to the existing slots).
	 *
	 * @param pass Render pass that draws them.
	 * @param batches Triangles per color.
	 */
	void commit(const std::shared_ptr<RenderPass> &pass, const std::vector<ColorBatch> &batches);

	/// forget all renderables (the pass was cleared)
	void reset();

private:
	struct Slot {
		Eigen::Vector4f color;
		std::shared_ptr<Geometry> geometry;
		std::shared_ptr<UniformInput> uniforms;
		size_t vertices = 0;
	};
	void drop(const std::shared_ptr<RenderPass> &pass, Slot &slot);

	std::shared_ptr<Renderer> renderer;
	std::shared_ptr<ShaderProgram> shader;
	std::vector<Slot> slot_list;
};

} // namespace hud
} // namespace openage::renderer
