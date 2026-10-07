// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "render_stage.h"

#include <algorithm>
#include <chrono>

#include <epoxy/gl.h>

#include "log/log.h"
#include "renderer/geometry.h"
#include "renderer/opengl/context.h"
#include "renderer/opengl/texture.h"
#include "renderer/render_pass.h"
#include "renderer/render_target.h"
#include "renderer/renderer.h"
#include "renderer/resources/mesh_data.h"
#include "renderer/resources/shader_source.h"
#include "renderer/resources/texture_info.h"
#include "renderer/shader_program.h"
#include "renderer/texture.h"
#include "renderer/uniform_input.h"
#include "renderer/window.h"
#include "ui/game_ui_controller.h"


namespace openage::renderer::ui {

UiRenderStage::UiRenderStage(const std::shared_ptr<Window> &window,
                             const std::shared_ptr<renderer::Renderer> &renderer,
                             const util::Path &shaderdir,
                             const std::shared_ptr<openage::ui::GameUiController> &controller) :
	renderer{renderer},
	controller{controller} {
	renderer::opengl::GlContext::check_error();

	auto size = window->get_size();
	this->width = size[0];
	this->height = size[1];
	this->controller->resize(this->width, this->height);
	this->initialize_render_pass(size[0], size[1], shaderdir);

	window->add_resize_callback([this](size_t w, size_t h, double /*scale*/) {
		this->resize(w, h);
	});

	log::log(INFO << "Created render stage 'UI'");
}

std::shared_ptr<renderer::RenderPass> UiRenderStage::get_render_pass() {
	return this->render_pass;
}

void UiRenderStage::initialize_render_pass(size_t width, size_t height, const util::Path &shaderdir) {
	auto vert_shader_file = (shaderdir / "ui_overlay.vert.glsl").open();
	auto vert_shader_src = renderer::resources::ShaderSource(
		resources::shader_lang_t::glsl,
		resources::shader_stage_t::vertex,
		vert_shader_file.read());
	vert_shader_file.close();

	auto frag_shader_file = (shaderdir / "ui_overlay.frag.glsl").open();
	auto frag_shader_src = renderer::resources::ShaderSource(
		resources::shader_lang_t::glsl,
		resources::shader_stage_t::fragment,
		frag_shader_file.read());
	frag_shader_file.close();

	this->shader = this->renderer->add_shader({vert_shader_src, frag_shader_src});
	this->quad = this->renderer->add_mesh_geometry(resources::MeshData::make_quad());

	// HUD bar: fixed texture size, drawn scaled to the window width
	this->hud_texture = this->renderer->add_texture(resources::Texture2dInfo(
		openage::ui::GameUiController::hud_texture_width(),
		openage::ui::GameUiController::hud_texture_height(),
		resources::pixel_format::rgba8));
	this->hud_uniforms = this->shader->new_uniform_input("tex", this->hud_texture);
	this->create_overlay_texture(width, height);

	this->output_texture = this->renderer->add_texture(resources::Texture2dInfo(width, height, resources::pixel_format::rgba8));
	auto fbo = this->renderer->create_texture_target({this->output_texture});
	this->render_pass = this->renderer->add_render_pass({}, fbo);
	// the overlay (menus) lies over the HUD bar
	this->render_pass->add_renderables(Renderable{this->hud_uniforms, this->quad, true, false});
	this->render_pass->add_renderables(Renderable{this->overlay_uniforms, this->quad, true, false});
	this->update_rects();
}

void UiRenderStage::create_overlay_texture(size_t width, size_t height) {
	this->overlay_texture = this->renderer->add_texture(
		resources::Texture2dInfo(std::max<size_t>(width, 16), std::max<size_t>(height, 16), resources::pixel_format::rgba8));
	if (this->overlay_uniforms) {
		this->overlay_uniforms->update("tex", this->overlay_texture);
	}
	else {
		this->overlay_uniforms = this->shader->new_uniform_input("tex", this->overlay_texture);
	}
	// new texture: everything has to be uploaded again
	this->overlay_shown = 0;
}

void UiRenderStage::update_rects() {
	int x0, y0, x1, y1;
	this->controller->hud_screen_rect(x0, y0, x1, y1);
	const float w = static_cast<float>(std::max<size_t>(this->width, 1));
	const float h = static_cast<float>(std::max<size_t>(this->height, 1));
	// window pixels (origin top left) -> NDC (origin bottom left): left, bottom, right, top
	auto ndc = [w, h](int px, int py) {
		return Eigen::Vector2f{2.0f * static_cast<float>(px) / w - 1.0f, 1.0f - 2.0f * static_cast<float>(py) / h};
	};
	auto top_left = ndc(x0, y0);
	auto bottom_right = ndc(x1, y1);
	this->hud_uniforms->update("rect", Eigen::Vector4f{top_left.x(), bottom_right.y(), bottom_right.x(), top_left.y()});
	this->overlay_uniforms->update("rect", Eigen::Vector4f{-1.0f, -1.0f, 1.0f, 1.0f});
}

void UiRenderStage::resize(size_t width, size_t height) {
	this->width = width;
	this->height = height;
	this->controller->resize(width, height);
	this->create_overlay_texture(width, height);
	this->output_texture = this->renderer->add_texture(resources::Texture2dInfo(width, height, resources::pixel_format::rgba8));
	auto fbo = this->renderer->create_texture_target({this->output_texture});
	this->render_pass->set_target(fbo);
	this->update_rects();
}

void UiRenderStage::upload_rows(const std::shared_ptr<Texture2d> &texture,
                                const uint32_t *pixels,
                                int width,
                                int y0,
                                int y1) {
	if (pixels == nullptr or y1 <= y0) {
		return;
	}
	auto gl_texture = std::dynamic_pointer_cast<opengl::GlTexture2d>(texture);
	if (not gl_texture) {
		return;
	}
	// canvas row 0 (top) lands in texture row 0 (OpenGL: bottom); the fragment shader flips v
	glBindTexture(GL_TEXTURE_2D, gl_texture->get_handle());
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, width, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE,
	                pixels + static_cast<size_t>(y0) * static_cast<size_t>(width));
	glBindTexture(GL_TEXTURE_2D, 0);
}

void UiRenderStage::update(double now) {
	this->controller->update(now);

	const uint32_t *hud_pixels = this->controller->hud_pixels(now);
	const int hud_w = openage::ui::GameUiController::hud_texture_width();
	const int hud_h = openage::ui::GameUiController::hud_texture_height();
	if (this->controller->hud_version() != this->hud_shown) {
		if (this->hud_shown == 0) {
			this->upload_rows(this->hud_texture, hud_pixels, hud_w, 0, hud_h);
		}
		else {
			for (const auto &band : this->controller->hud_bands()) {
				this->upload_rows(this->hud_texture, hud_pixels, hud_w, band.y0, band.y1);
			}
		}
		this->hud_shown = this->controller->hud_version();
	}

	const uint32_t *overlay_pixels = this->controller->overlay_pixels();
	const int overlay_w = this->controller->overlay_width();
	const int overlay_h = this->controller->overlay_height();
	if (overlay_pixels != nullptr and this->controller->overlay_version() != this->overlay_shown
	    and overlay_w == static_cast<int>(this->overlay_texture->get_info().get_size().first)
	    and overlay_h == static_cast<int>(this->overlay_texture->get_info().get_size().second)) {
		if (this->overlay_shown == 0) {
			this->upload_rows(this->overlay_texture, overlay_pixels, overlay_w, 0, overlay_h);
		}
		else {
			this->upload_rows(this->overlay_texture, overlay_pixels, overlay_w,
			                  this->controller->overlay_dirty_y0(), this->controller->overlay_dirty_y1());
		}
		this->overlay_shown = this->controller->overlay_version();
	}
}

} // namespace openage::renderer::ui
