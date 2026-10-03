// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <memory>
#include <string>

#include "renderer/window.h"


namespace openage::renderer {

class FrameSink;

namespace opengl {

class GlContext;

/**
 * Window without a window (XR fork): renders into the frames of a FrameSink
 * provided by an embedder (renderer/frame_sink.h).
 *
 * Creates an own OpenGL ES 3.2 (fallback 3.0) EGL context on the display of
 * the sink, shared with the context of the sink, and makes it current without
 * a surface (EGL_KHR_surfaceless_context, otherwise a 1x1 pbuffer). The
 * framebuffer of the current sink slot is the default framebuffer of the
 * renderer, so the final pass draws directly into the slot texture.
 *
 * Has to be created, used and destroyed in the presenter thread.
 */
class EglSinkWindow final : public Window {
public:
	/**
	 * Create the EGL context and acquire the first frame of the sink.
	 *
	 * @param title Name for log messages.
	 * @param settings Window settings, settings.sink must be set.
	 */
	EglSinkWindow(const std::string &title,
	              window_settings settings);
	~EglSinkWindow();

	EglSinkWindow(const EglSinkWindow &) = delete;
	EglSinkWindow &operator=(const EglSinkWindow &) = delete;

	/**
	 * Change the render size (the sink normally requests sizes with frame_size()).
	 * Calls the resize callbacks; the next acquired frame has the new size.
	 */
	void set_size(size_t width, size_t height) override;

	/**
	 * Publish the frame rendered since the last update(), deliver the sink's
	 * input events to the callbacks, apply size requests and acquire the
	 * framebuffer for the next frame.
	 */
	void update() override;

	std::shared_ptr<Renderer> make_renderer() override;

	/// Return the GL context of this window.
	const std::shared_ptr<GlContext> &get_context() const;

private:
	/// Acquire the sink framebuffer for the next frame in the current size.
	void acquire_frame();

	/// Destroy the EGL context and surface (if any).
	void destroy_egl();

	/// Consumer of the frames.
	std::shared_ptr<FrameSink> sink;

	/// EGLDisplay of the sink.
	void *egl_display = nullptr;

	/// Own EGLContext, shared with the context of the sink.
	void *egl_context = nullptr;

	/// 1x1 pbuffer (EGLSurface) if surfaceless contexts are not supported.
	void *egl_surface = nullptr;

	/// Renderer view of the EGL context.
	std::shared_ptr<GlContext> context;

	/// True between acquire_frame() and the publish() in update().
	bool frame_acquired = false;

	/// Number of published frames (for log messages).
	size_t frames_published = 0;
};

} // namespace opengl
} // namespace openage::renderer
