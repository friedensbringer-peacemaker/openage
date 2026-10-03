// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "egl_window.h"

#include <array>
#include <cstdio>
#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include "error/error.h"
#include "log/log.h"

#include "renderer/frame_sink.h"
#include "renderer/opengl/context.h"
#include "renderer/opengl/renderer.h"


namespace openage::renderer::opengl {

namespace {

/// Number of input events fetched from the sink per call.
constexpr size_t input_batch = 64;

/// OpenGL ES versions to try, the first one that works is used.
constexpr std::array<std::pair<EGLint, EGLint>, 2> gles_versions = {{{3, 2}, {3, 0}}};

/// EGL error code as hex string.
std::string egl_hex(EGLint code) {
	char text[16];
	std::snprintf(text, sizeof(text), "0x%04x", static_cast<unsigned>(code));
	return text;
}

/// Find a config for the 1x1 pbuffer if the sink has none (EGL_KHR_no_config_context).
EGLConfig choose_pbuffer_config(EGLDisplay display) {
	const EGLint attribs[] = {
		EGL_RENDERABLE_TYPE,
		EGL_OPENGL_ES3_BIT,
		EGL_SURFACE_TYPE,
		EGL_PBUFFER_BIT,
		EGL_RED_SIZE,
		8,
		EGL_GREEN_SIZE,
		8,
		EGL_BLUE_SIZE,
		8,
		EGL_ALPHA_SIZE,
		8,
		EGL_NONE,
	};
	EGLConfig config = nullptr;
	EGLint count = 0;
	if (not eglChooseConfig(display, attribs, &config, 1, &count) or count < 1) {
		return nullptr;
	}
	return config;
}

} // namespace


EglSinkWindow::EglSinkWindow(const std::string &title,
                             window_settings settings) :
	Window{settings.width, settings.height},
	sink{settings.sink} {
	if (not this->sink) {
		throw Error{MSG(err) << "EGL sink window '" << title << "': no frame sink given."};
	}

	// the sink decides the size if it requests one
	int width = 0;
	int height = 0;
	this->sink->frame_size(width, height);
	if (width > 0 and height > 0) {
		this->size = {static_cast<size_t>(width), static_cast<size_t>(height)};
	}

	EGLDisplay display = static_cast<EGLDisplay>(this->sink->egl_display());
	if (display == EGL_NO_DISPLAY) {
		throw Error{MSG(err) << "EGL sink window '" << title << "': the sink has no EGL display."};
	}
	this->egl_display = display;

	// the API is a per-thread setting
	if (not eglBindAPI(EGL_OPENGL_ES_API)) {
		throw Error{MSG(err) << "eglBindAPI(OpenGL ES) failed: " << egl_hex(eglGetError())};
	}

	EGLConfig config = static_cast<EGLConfig>(this->sink->egl_config());
	EGLContext share = static_cast<EGLContext>(this->sink->share_context());

	EGLContext context = EGL_NO_CONTEXT;
	EGLint error = EGL_SUCCESS;
	for (auto [major, minor] : gles_versions) {
		const EGLint attribs[] = {
			EGL_CONTEXT_MAJOR_VERSION,
			major,
			EGL_CONTEXT_MINOR_VERSION,
			minor,
			EGL_NONE,
		};
		context = eglCreateContext(display, config, share, attribs);
		if (context != EGL_NO_CONTEXT) {
			break;
		}
		error = eglGetError();
		log::log(MSG(warn) << "EGL sink window: no OpenGL ES " << major << "." << minor
		                   << " context (" << egl_hex(error) << ")");
	}
	if (context == EGL_NO_CONTEXT) {
		throw Error{MSG(err) << "EGL sink window '" << title << "': eglCreateContext failed: "
		                     << egl_hex(error)};
	}
	this->egl_context = context;

	// no window: surfaceless if possible, otherwise a tiny pbuffer that is never drawn to
	EGLSurface surface = EGL_NO_SURFACE;
	const bool surfaceless = epoxy_has_egl_extension(display, "EGL_KHR_surfaceless_context");
	if (not surfaceless) {
		EGLConfig pbuffer_config = config != nullptr ? config : choose_pbuffer_config(display);
		const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
		surface = eglCreatePbufferSurface(display, pbuffer_config, pbuffer_attribs);
		if (surface == EGL_NO_SURFACE) {
			error = eglGetError();
			this->destroy_egl();
			throw Error{MSG(err) << "EGL sink window '" << title
			                     << "': no surfaceless contexts and no pbuffer: " << egl_hex(error)};
		}
		this->egl_surface = surface;
	}

	if (not eglMakeCurrent(display, surface, surface, context)) {
		error = eglGetError();
		this->destroy_egl();
		throw Error{MSG(err) << "EGL sink window '" << title << "': eglMakeCurrent failed: "
		                     << egl_hex(error)};
	}

	try {
		// the slot framebuffer is set for every frame
		this->context = std::make_shared<GlContext>(true, 0);

		log::log(MSG(info) << "Created EGL sink window '" << title << "' "
		                   << this->size[0] << "x" << this->size[1]
		                   << (surfaceless ? " (surfaceless)" : " (pbuffer)"));

		this->acquire_frame();
		GlContext::check_error();
	}
	catch (...) {
		// the destructor does not run for a failed constructor
		this->destroy_egl();
		throw;
	}
}


EglSinkWindow::~EglSinkWindow() {
	// the renderer and all GL objects are gone: the presenter destroys its
	// stages before the window, which every stage holds
	log::log(MSG(info) << "EGL sink window: " << this->frames_published << " frames published");
	this->destroy_egl();
}


void EglSinkWindow::set_size(size_t width, size_t height) {
	this->size = {width, height};

	for (auto &cb : this->on_resize) {
		cb(width, height, this->scale_dpr);
	}
}


void EglSinkWindow::update() {
	if (this->frame_acquired) {
		// fence + flush, waits until the consumer took the frame (pacing)
		this->sink->publish();
		this->frame_acquired = false;
		this->frames_published += 1;
	}

	// input of the sink -> window events -> callbacks
	std::array<SinkInputEvent, input_batch> events;
	size_t count = 0;
	do {
		count = this->sink->poll_input(events.data(), events.size());
		for (size_t i = 0; i < count; ++i) {
			WindowEvent ev;
			if (not to_window_event(events[i], ev)) {
				continue;
			}
			switch (ev.type) {
			case input::event_type::KeyPress:
			case input::event_type::KeyRelease:
				for (auto &cb : this->on_key) {
					cb(ev);
				}
				break;
			case input::event_type::MouseButtonPress:
			case input::event_type::MouseButtonRelease:
			case input::event_type::MouseButtonDblClick:
				for (auto &cb : this->on_mouse_button) {
					cb(ev);
				}
				break;
			case input::event_type::MouseMove:
				for (auto &cb : this->on_mouse_move) {
					cb(ev);
				}
				break;
			case input::event_type::Wheel:
				for (auto &cb : this->on_mouse_wheel) {
					cb(ev);
				}
				break;
			default:
				break;
			}
		}
	}
	while (count == events.size());

	if (this->sink->should_close()) {
		this->should_be_closed = true;
		return;
	}

	// size requests of the sink (e.g. render resolution from the VR menu)
	int width = 0;
	int height = 0;
	this->sink->frame_size(width, height);
	if (width > 0 and height > 0
	    and (static_cast<size_t>(width) != this->size[0] or static_cast<size_t>(height) != this->size[1])) {
		log::log(MSG(info) << "EGL sink window: resize " << this->size[0] << "x" << this->size[1]
		                   << " -> " << width << "x" << height);
		this->set_size(width, height);
	}

	this->acquire_frame();
}


std::shared_ptr<Renderer> EglSinkWindow::make_renderer() {
	auto renderer = std::make_shared<GlRenderer>(this->get_context(), this->size);

	this->add_resize_callback([renderer](size_t w, size_t h, double /*scale*/) {
		renderer->resize_display_target(w, h);
	});

	return renderer;
}


const std::shared_ptr<GlContext> &EglSinkWindow::get_context() const {
	return this->context;
}


void EglSinkWindow::acquire_frame() {
	unsigned fbo = this->sink->acquire_target(static_cast<int>(this->size[0]),
	                                          static_cast<int>(this->size[1]));
	if (fbo == 0) {
		throw Error{MSG(err) << "EGL sink window: no frame of size "
		                     << this->size[0] << "x" << this->size[1] << " from the sink."};
	}
	this->context->set_default_framebuffer_id(fbo);
	this->frame_acquired = true;
}


void EglSinkWindow::destroy_egl() {
	EGLDisplay display = static_cast<EGLDisplay>(this->egl_display);
	if (display == EGL_NO_DISPLAY) {
		return;
	}

	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (this->egl_surface != nullptr) {
		eglDestroySurface(display, static_cast<EGLSurface>(this->egl_surface));
		this->egl_surface = nullptr;
	}
	if (this->egl_context != nullptr) {
		eglDestroyContext(display, static_cast<EGLContext>(this->egl_context));
		this->egl_context = nullptr;
	}
	eglReleaseThread();
}

} // namespace openage::renderer::opengl
