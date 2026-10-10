// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "color_batches.h"

#include <cstring>

#include "renderer/geometry.h"
#include "renderer/render_pass.h"
#include "renderer/renderer.h"
#include "renderer/resources/mesh_data.h"
#include "renderer/shader_program.h"
#include "renderer/uniform_input.h"


namespace openage::renderer::hud {

void ColorBatch::tri(float x0, float y0, float x1, float y1, float x2, float y2) {
	const float v[6] = {x0, y0, x1, y1, x2, y2};
	this->verts.insert(this->verts.end(), v, v + 6);
}

void ColorBatch::rect(float l, float b, float r, float t) {
	this->tri(l, b, r, b, r, t);
	this->tri(l, b, r, t, l, t);
}

void ColorBatch::quad(const Eigen::Vector2f &a, const Eigen::Vector2f &b, const Eigen::Vector2f &c, const Eigen::Vector2f &d) {
	this->tri(a.x(), a.y(), b.x(), b.y(), c.x(), c.y());
	this->tri(a.x(), a.y(), c.x(), c.y(), d.x(), d.y());
}

void ColorBatch::ring(const std::vector<Eigen::Vector2f> &points, float wx, float wy) {
	const size_t n = points.size();
	if (n < 3) {
		return;
	}
	// each edge as a band: offset outwards and inwards by half the width (normal in pixels)
	for (size_t i = 0; i < n; ++i) {
		const Eigen::Vector2f &p = points[i];
		const Eigen::Vector2f &q = points[(i + 1) % n];
		Eigen::Vector2f d{(q.x() - p.x()) / wx, (q.y() - p.y()) / wy};
		const float len = d.norm();
		if (len <= 0.0f) {
			continue;
		}
		Eigen::Vector2f nrm{-d.y() / len * wx * 0.5f, d.x() / len * wy * 0.5f};
		// extend the band a bit along the edge so that the corners close
		Eigen::Vector2f ext{d.x() / len * wx * 0.5f, d.y() / len * wy * 0.5f};
		this->quad(p + nrm - ext, q + nrm + ext, q - nrm + ext, p - nrm - ext);
	}
}

ColorBatches::ColorBatches(const std::shared_ptr<Renderer> &renderer, const std::shared_ptr<ShaderProgram> &shader) :
	renderer{renderer},
	shader{shader} {}

void ColorBatches::reset() {
	this->slot_list.clear();
}

void ColorBatches::drop(const std::shared_ptr<RenderPass> &pass, Slot &slot) {
	if (slot.uniforms) {
		auto old = slot.uniforms;
		pass->remove_renderables([&old](const Renderable &renderable) {
			return renderable.uniform == old;
		});
	}
	slot.geometry = nullptr;
	slot.uniforms = nullptr;
	slot.vertices = 0;
}

void ColorBatches::commit(const std::shared_ptr<RenderPass> &pass, const std::vector<ColorBatch> &batches) {
	std::vector<bool> used(this->slot_list.size(), false);
	for (const auto &batch : batches) {
		if (batch.verts.empty()) {
			continue;
		}
		// slot of this color (exact match), else a new one
		size_t index = this->slot_list.size();
		for (size_t i = 0; i < this->slot_list.size(); ++i) {
			if (not used[i] and this->slot_list[i].color == batch.color) {
				index = i;
				break;
			}
		}
		if (index == this->slot_list.size()) {
			this->slot_list.push_back(Slot{batch.color, nullptr, nullptr, 0});
			used.push_back(false);
		}
		used[index] = true;
		Slot &slot = this->slot_list[index];
		const size_t count = batch.verts.size() / 2;
		std::vector<uint8_t> data(batch.verts.size() * sizeof(float));
		std::memcpy(data.data(), batch.verts.data(), data.size());
		if (count == slot.vertices and slot.geometry) {
			slot.geometry->update_verts(data);
			continue;
		}
		this->drop(pass, slot);
		resources::VertexInputInfo info{{resources::vertex_input_t::V2F32},
		                                resources::vertex_layout_t::AOS,
		                                resources::vertex_primitive_t::TRIANGLES};
		slot.geometry = this->renderer->add_mesh_geometry(resources::MeshData{std::move(data), info});
		slot.uniforms = this->shader->new_uniform_input("in_col", slot.color);
		slot.vertices = count;
		pass->add_renderables(Renderable{slot.uniforms, slot.geometry, true, false});
	}
	// colors not shown any more
	for (size_t i = 0; i < this->slot_list.size(); ++i) {
		if (not used[i] and this->slot_list[i].geometry) {
			this->drop(pass, this->slot_list[i]);
		}
	}
}

} // namespace openage::renderer::hud
