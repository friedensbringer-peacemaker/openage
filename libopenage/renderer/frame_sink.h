// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <cstddef>

#include "renderer/window_events.h"


namespace openage::renderer {

/**
 * Input event delivered by a frame sink (XR fork).
 *
 * Same layout, semantics and numeric values as agesxr::SinkInputEvent of the
 * Quest app (native/app/frame_sink.h): keys, buttons and modifiers use the
 * values of Qt, see input/keys.h.
 */
struct SinkInputEvent {
	enum Type : int {
		kMouseMove = 0,
		kMouseDown = 1,
		kMouseUp = 2,
		/// wheel: steps, + = away from the user (like Qt angleDelta / 120)
		kWheel = 3,
		kKeyDown = 4,
		kKeyUp = 5,
		/// key: 1 = focus gained, 0 = lost (system menu, headset taken off)
		kFocus = 6,
		/// pointer left the image (no more hover)
		kLeave = 7,
	};

	// Qt::MouseButton
	static constexpr int kLeftButton = 0x1;
	static constexpr int kRightButton = 0x2;
	static constexpr int kMiddleButton = 0x4;
	// Qt::Key
	static constexpr int kKeyEscape = 0x01000000;
	static constexpr int kKeyControl = 0x01000021;
	// Qt::KeyboardModifier
	static constexpr int kControlModifier = 0x04000000;

	int type = kMouseMove;
	/// image pixels, origin top left
	int x = 0, y = 0;
	/// kMouseDown/kMouseUp
	int button = 0;
	/// buttons held after the event (mask)
	int buttons = 0;
	/// kKeyDown/kKeyUp/kFocus
	int key = 0;
	int modifiers = 0;
	float wheel = 0.0f;
};

/**
 * Consumer of the rendered frames, provided by an embedder that owns the
 * display (e.g. the OpenXR layer of the Quest app). Without EGL, GL or OpenXR
 * types: handles are void*, the framebuffer is a GLuint as unsigned.
 *
 * Same methods and semantics as agesxr::FrameSink (native/app/frame_sink.h),
 * so the embedder can implement this interface directly.
 *
 * Usage by the producer (EglSinkWindow in the presenter thread):
 *   1. Create an own EGL context: eglCreateContext(egl_display(), egl_config(),
 *      share_context(), ES 3.2) and make it current without a surface.
 *   2. Per frame: frame_size(w, h) -> fbo = acquire_target(w, h) -> render
 *      into fbo (origin bottom left) -> publish(). publish() places a GPU fence,
 *      flushes and waits until the consumer took the frame (replaces vsync).
 *   3. poll_input() empties the input queue (mouse coordinates in image
 *      pixels, origin top left, relative to the last shown image).
 *   4. should_close() -> leave the draw loop, release and destroy the context.
 *      paused() -> pause the simulation (rendering may continue).
 *
 * All functions except egl_*(), frame_size(), should_close() and paused()
 * are only called from the producer thread.
 */
class FrameSink {
public:
	virtual ~FrameSink() = default;

	/// EGLDisplay
	virtual void *egl_display() const = 0;
	/// EGLConfig of the consumer context (ES 3, RGBA8), may be EGL_NO_CONFIG_KHR
	virtual void *egl_config() const = 0;
	/// EGLContext of the consumer (textures are shared with it)
	virtual void *share_context() const = 0;

	/// Requested render size. Changes atomically; the producer applies it with
	/// the next acquire_target().
	virtual void frame_size(int &width, int &height) const = 0;

	/// Framebuffer (GLuint) of the next free slot with size width x height, valid in
	/// the context of the caller until publish(). RGBA8 color, no depth. 0 = error.
	virtual unsigned acquire_target(int width, int height) = 0;
	virtual void publish() = 0;

	/// Copy up to max events to out; returns the number of events.
	virtual std::size_t poll_input(SinkInputEvent *out, std::size_t max) = 0;

	virtual bool should_close() const = 0;
	virtual bool paused() const = 0;
};


/**
 * Convert a sink input event into a window event (the only place of this mapping).
 *
 * @param in Event of the sink.
 * @param out Window event (type, key, buttons, modifiers, position, wheel).
 *
 * @return false if the event has no window event equivalent (focus, leave).
 */
bool to_window_event(const SinkInputEvent &in, WindowEvent &out);

} // namespace openage::renderer
