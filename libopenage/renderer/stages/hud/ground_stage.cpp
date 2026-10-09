// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "ground_stage.h"

#include "log/log.h"
#include "renderer/opengl/context.h"
#include "renderer/render_pass.h"
#include "renderer/render_target.h"
#include "renderer/renderer.h"
#include "renderer/resources/shader_source.h"
#include "renderer/resources/texture_info.h"
#include "renderer/shader_program.h"
#include "renderer/texture.h"
#include "renderer/window.h"


namespace openage::renderer::hud {

GroundMarkerStage::GroundMarkerStage(const std::shared_ptr<Window> &window,
                                     const std::shared_ptr<renderer::Renderer> &renderer,
                                     const util::Path &shaderdir) :
	renderer{renderer} {
	renderer::opengl::GlContext::check_error();

	// same flat color shader as the drag rectangle of the HUD
	auto vert_file = (shaderdir / "hud_drag_select.vert.glsl").open();
	auto vert_src = resources::ShaderSource(resources::shader_lang_t::glsl,
	                                        resources::shader_stage_t::vertex,
	                                        vert_file.read());
	vert_file.close();
	auto frag_file = (shaderdir / "hud_drag_select.frag.glsl").open();
	auto frag_src = resources::ShaderSource(resources::shader_lang_t::glsl,
	                                        resources::shader_stage_t::fragment,
	                                        frag_file.read());
	frag_file.close();
	this->shader = this->renderer->add_shader({vert_src, frag_src});

	auto size = window->get_size();
	this->output_texture = renderer->add_texture(resources::Texture2dInfo(size[0], size[1], resources::pixel_format::rgba8));
	this->depth_texture = renderer->add_texture(resources::Texture2dInfo(size[0], size[1], resources::pixel_format::depth24));
	auto fbo = this->renderer->create_texture_target({this->output_texture, this->depth_texture});
	this->render_pass = this->renderer->add_render_pass({}, fbo);
	this->batches = std::make_unique<ColorBatches>(this->renderer, this->shader);

	window->add_resize_callback([this](size_t width, size_t height, double /*scale*/) {
		this->resize(width, height);
	});

	log::log(INFO << "Created render stage 'Ground markers'");
}

std::shared_ptr<renderer::RenderPass> GroundMarkerStage::get_render_pass() {
	return this->render_pass;
}

void GroundMarkerStage::set_batches(std::vector<ColorBatch> &&next) {
	std::lock_guard<std::mutex> lock{this->mutex};
	this->pending = std::move(next);
	this->changed = true;
}

void GroundMarkerStage::update() {
	std::lock_guard<std::mutex> lock{this->mutex};
	if (not this->changed) {
		return;
	}
	this->changed = false;
	this->batches->commit(this->render_pass, this->pending);
}

void GroundMarkerStage::resize(size_t width, size_t height) {
	this->output_texture = renderer->add_texture(resources::Texture2dInfo(width, height, resources::pixel_format::rgba8));
	this->depth_texture = renderer->add_texture(resources::Texture2dInfo(width, height, resources::pixel_format::depth24));
	auto fbo = this->renderer->create_texture_target({this->output_texture, this->depth_texture});
	this->render_pass->set_target(fbo);
}

} // namespace openage::renderer::hud
