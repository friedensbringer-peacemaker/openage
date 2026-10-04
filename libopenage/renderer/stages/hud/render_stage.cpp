// Copyright 2023-2024 the openage authors. See copying.md for legal info.

#include "render_stage.h"

#include <algorithm>
#include <cstring>

#include "renderer/geometry.h"
#include "renderer/resources/mesh_data.h"
#include "renderer/uniform_input.h"

#include "renderer/camera/camera.h"
#include "renderer/opengl/context.h"
#include "renderer/render_pass.h"
#include "renderer/render_target.h"
#include "renderer/resources/assets/asset_manager.h"
#include "renderer/resources/shader_source.h"
#include "renderer/resources/texture_info.h"
#include "renderer/shader_program.h"
#include "renderer/stages/hud/object.h"
#include "renderer/texture.h"
#include "renderer/window.h"
#include "time/clock.h"


namespace openage::renderer::hud {

HudRenderStage::HudRenderStage(const std::shared_ptr<Window> &window,
                               const std::shared_ptr<renderer::Renderer> &renderer,
                               const std::shared_ptr<renderer::camera::Camera> &camera,
                               const util::Path &shaderdir,
                               const std::shared_ptr<renderer::resources::AssetManager> &asset_manager,
                               const std::shared_ptr<time::Clock> clock) :
	renderer{renderer},
	camera{camera},
	asset_manager{asset_manager},
	drag_object{nullptr},
	clock{clock} {
	renderer::opengl::GlContext::check_error();

	auto size = window->get_size();
	this->initialize_render_pass(size[0], size[1], shaderdir);

	window->add_resize_callback([this](size_t width, size_t height, double /*scale*/) {
		this->resize(width, height);
	});

	log::log(INFO << "Created render stage 'HUD'");
}

std::shared_ptr<renderer::RenderPass> HudRenderStage::get_render_pass() {
	return this->render_pass;
}

void HudRenderStage::add_drag_entity(const std::shared_ptr<DragRenderEntity> entity) {
	std::unique_lock lock{this->mutex};

	auto hud_object = std::make_shared<HudDragObject>(this->asset_manager);
	hud_object->set_render_entity(entity);
	hud_object->set_camera(this->camera);
	this->drag_object = hud_object;
}

void HudRenderStage::remove_drag_entity() {
	std::unique_lock lock{this->mutex};

	this->drag_object = nullptr;
	this->render_pass->clear_renderables();
	// XR fork: the selection frames were cleared as well
	this->marker_geometry = {};
	this->marker_uniforms = {};
	this->marker_vertices = {0, 0};
}

void HudRenderStage::set_selection_markers(std::vector<SelectionMarker> &&markers) {
	std::unique_lock lock{this->mutex};
	this->selection_markers = std::move(markers);
}

void HudRenderStage::update_selection_markers() {
	// frame width 2 px, 3 px away from the sprite
	auto viewport = this->camera->get_viewport_size();
	const float px_x = 2.0f / static_cast<float>(std::max<size_t>(viewport[0], 1));
	const float px_y = 2.0f / static_cast<float>(std::max<size_t>(viewport[1], 1));
	const float line_x = 2.0f * px_x;
	const float line_y = 2.0f * px_y;
	const float pad_x = 3.0f * px_x;
	const float pad_y = 3.0f * px_y;

	for (size_t group = 0; group < 2; ++group) {
		const bool own = group == 0;
		// 4 edges as 2 triangles each
		std::vector<float> verts;
		auto rect = [&verts](float l, float b, float r, float t) {
			const float quad[12] = {l, b, r, b, r, t, l, b, r, t, l, t};
			verts.insert(verts.end(), quad, quad + 12);
		};
		for (const auto &marker : this->selection_markers) {
			if (marker.own != own) {
				continue;
			}
			float l = marker.ndc.x() - pad_x;
			float b = marker.ndc.y() - pad_y;
			float r = marker.ndc.z() + pad_x;
			float t = marker.ndc.w() + pad_y;
			rect(l, b, r, b + line_y);
			rect(l, t - line_y, r, t);
			rect(l, b, l + line_x, t);
			rect(r - line_x, b, r, t);
		}

		const size_t vertex_count = verts.size() / 2;
		if (vertex_count != this->marker_vertices[group]) {
			// size changed: new geometry, the old renderable leaves the pass
			if (this->marker_uniforms[group]) {
				auto old = this->marker_uniforms[group];
				this->render_pass->remove_renderables([&old](const Renderable &renderable) {
					return renderable.uniform == old;
				});
			}
			this->marker_geometry[group] = nullptr;
			this->marker_uniforms[group] = nullptr;
			this->marker_vertices[group] = vertex_count;
			if (vertex_count == 0) {
				continue;
			}
			std::vector<uint8_t> data(verts.size() * sizeof(float));
			std::memcpy(data.data(), verts.data(), data.size());
			resources::VertexInputInfo info{{resources::vertex_input_t::V2F32},
			                                resources::vertex_layout_t::AOS,
			                                resources::vertex_primitive_t::TRIANGLES};
			this->marker_geometry[group] = this->renderer->add_mesh_geometry(
				resources::MeshData{std::move(data), info});
			this->marker_uniforms[group] = this->drag_select_shader->new_uniform_input(
				"in_col",
				own ? Eigen::Vector4f{1.0f, 1.0f, 1.0f, 0.9f} : Eigen::Vector4f{1.0f, 0.85f, 0.2f, 0.9f});
			this->render_pass->add_renderables(Renderable{
				this->marker_uniforms[group],
				this->marker_geometry[group],
				true,
				false,
			});
		}
		else if (vertex_count > 0) {
			std::vector<uint8_t> data(verts.size() * sizeof(float));
			std::memcpy(data.data(), verts.data(), data.size());
			this->marker_geometry[group]->update_verts(data);
		}
	}
}

void HudRenderStage::update() {
	std::unique_lock lock{this->mutex};
	auto current_time = this->clock->get_real_time();

	if (this->drag_object) {
		this->drag_object->fetch_updates(current_time);
		if (this->drag_object->requires_renderable()) {
			auto geometry = this->renderer->add_mesh_geometry(resources::MeshData::make_quad());
			auto transform_unifs = this->drag_select_shader->new_uniform_input(
				"in_col",
				Eigen::Vector4f{0.0f, 0.0f, 0.0f, 0.2f});

			Renderable display_obj{
				transform_unifs,
				geometry,
				true,
				true,
			};

			this->render_pass->add_renderables(std::move(display_obj));
			this->drag_object->clear_requires_renderable();

			this->drag_object->set_uniforms(transform_unifs);
			this->drag_object->set_geometry(geometry);
		}
		this->drag_object->update_uniforms(current_time);
		this->drag_object->update_geometry(current_time);
	}

	this->update_selection_markers();
}

void HudRenderStage::resize(size_t width, size_t height) {
	this->output_texture = renderer->add_texture(resources::Texture2dInfo(width, height, resources::pixel_format::rgba8));
	this->depth_texture = renderer->add_texture(resources::Texture2dInfo(width, height, resources::pixel_format::depth24));

	auto fbo = this->renderer->create_texture_target({this->output_texture, this->depth_texture});
	this->render_pass->set_target(fbo);
}

void HudRenderStage::initialize_render_pass(size_t width,
                                            size_t height,
                                            const util::Path &shaderdir) {
	// Drag select shader
	auto vert_shader_file = (shaderdir / "hud_drag_select.vert.glsl").open();
	auto vert_shader_src = renderer::resources::ShaderSource(
		resources::shader_lang_t::glsl,
		resources::shader_stage_t::vertex,
		vert_shader_file.read());
	vert_shader_file.close();

	auto frag_shader_file = (shaderdir / "hud_drag_select.frag.glsl").open();
	auto frag_shader_src = renderer::resources::ShaderSource(
		resources::shader_lang_t::glsl,
		resources::shader_stage_t::fragment,
		frag_shader_file.read());
	frag_shader_file.close();

	this->drag_select_shader = this->renderer->add_shader({vert_shader_src, frag_shader_src});

	// Texture targets
	this->output_texture = renderer->add_texture(resources::Texture2dInfo(width, height, resources::pixel_format::rgba8));
	this->depth_texture = renderer->add_texture(resources::Texture2dInfo(width, height, resources::pixel_format::depth24));

	auto fbo = this->renderer->create_texture_target({this->output_texture, this->depth_texture});
	this->render_pass = this->renderer->add_render_pass({}, fbo);
}

} // namespace openage::renderer::hud
