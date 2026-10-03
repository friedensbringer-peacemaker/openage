// Copyright 2015-2026 the openage authors. See copying.md for legal info.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "config.h"

#if WITH_QT
	#include <QObject>
#endif

#include "renderer/renderer.h"
#include "renderer/types.h"
#include "renderer/window_events.h"
#include "util/vector.h"

#if WITH_QT
QT_FORWARD_DECLARE_CLASS(QWindow)
#endif

namespace openage::renderer {

class FrameSink;
class WindowEventHandler;

/**
 * Modes for window display.
 */
enum class window_mode {
	FULLSCREEN,
	BORDERLESS,
	WINDOWED
};

/**
 * Settings for creating a window.
 */
struct window_settings {
	// Width of the window in pixels.
	size_t width = 1024;
	// Height of the window in pixels.
	size_t height = 768;
	// Graphics API to use in the window's renderer.
	// OPENGL_ES selects an OpenGL ES 3.x context (also forced by the environment
	// variable OPENAGE_GLES=1), everything else desktop OpenGL.
	graphics_api_t backend = graphics_api_t::DEFAULT;
	// If true, enable vsync.
	bool vsync = true;
	// If true, enable debug logging for the selected backend.
	bool debug = false;
	// Display mode for the window.
	window_mode mode = window_mode::WINDOWED;
	// If false, the window is created but never shown (offscreen checks).
	bool visible = true;
	// If not empty: after capture_delay seconds of rendering, the presenter
	// renders the final frame into a texture, stores it as PNG to this file
	// and closes the window (offscreen render checks).
	std::string capture_file{};
	// Seconds to render before the frame is captured.
	double capture_delay = 10.0;
	// If set, the window renders into the frames of this sink (EGL context
	// shared with the embedder, no window of its own; see renderer/frame_sink.h)
	// instead of opening a window. The sink must outlive the presenter thread;
	// the shared_ptr keeps it alive (embedders that own the sink can pass a
	// shared_ptr with a no-op deleter).
	std::shared_ptr<FrameSink> sink{};
	// Color behind the map (skybox, RGBA 0..1; XR fork). The default is the
	// orange of upstream openage. Alpha 0 leaves the area around the map
	// transparent in the final frame, which is then premultiplied (rgb = 0
	// where alpha = 0), e.g. for an XR compositor layer in front of
	// passthrough. A frame sink can change it at runtime (poll_background()).
	std::array<float, 4> background{1.0f, 0.5f, 0.0f, 1.0f};
};


/**
 * Represents a window that can be used to display graphics.
 */
class Window {
public:
	/**
	 * Create a new Window instance for displaying stuff.
	 *
	 * @param title Window title shown in the Desktop Environment.
	 * @param settings Settings for creating the window.
	 *
	 * @return The created Window instance.
	 */
	static std::shared_ptr<Window> create(const std::string &title,
	                                      window_settings settings = {});

	virtual ~Window() = default;

	/**
	 * Get the dimensions of this window.
	 *
	 * @return (width, height) as a size-2 vector.
	 */
	const util::Vector2s &get_size() const;

	/**
	 * Get the scaling factor of the window.
	 *
	 * @return Scaling factor.
	 */
	double get_scale() const;

	/**
	 * Returns \p true if this window should be closed.
	 *
	 * @return true if the window should close, else false.
	 */
	bool should_close() const;

	// Input callbacks get window system independent events (renderer/window_events.h).
	using key_cb_t = std::function<void(const WindowEvent &)>;
	using mouse_button_cb_t = std::function<void(const WindowEvent &)>;
	using mouse_move_cb_t = std::function<void(const WindowEvent &)>;
	using mouse_wheel_cb_t = std::function<void(const WindowEvent &)>;
	using resize_cb_t = std::function<void(size_t, size_t, double)>;

	/**
	 * Register a function that executes when a key is pressed.
	 *
	 * @param cb Callback function.
	 */
	void add_key_callback(const key_cb_t &cb);

	/**
	 * Register a function that executes when a mouse button is pressed.
	 *
	 * @param cb Callback function.
	 */
	void add_mouse_button_callback(const mouse_button_cb_t &cb);

	/**
	 * Register a function that executes when the mouse is moved.
	 *
	 * @param cb Callback function.
	 */
	void add_mouse_move_callback(const mouse_move_cb_t &cb);

	/**
	 * Register a function that executes when a mouse wheel action is used.
	 *
	 * @param cb Callback function.
	 */
	void add_mouse_wheel_callback(const mouse_wheel_cb_t &cb);

	/**
	 * Register a function that executes when the window is resized.
	 *
	 * @param cb Callback function.
	 */
	void add_resize_callback(const resize_cb_t &cb);

#if WITH_QT
	/**
	 * Get the underlying QWindow that is used for drawing.
	 *
	 * @return Pointer to the QWindow.
	 */
	const std::shared_ptr<QWindow> &get_qt_window() const;
#endif

	/**
	 * Force this window to the given size. It's generally not a good idea to use this,
	 * as it makes the window jump around weirdly.
	 *
	 * @param width Width in pixels.
	 * @param height Height in pixels.
	 */
	virtual void set_size(size_t width, size_t height) = 0;

	/**
	 * Polls for window events, calls callbacks for these events, swaps front and back framebuffers
	 * to present graphics onto screen. This has to be called at the end of every graphics frame.
	 */
	virtual void update() = 0;

	/**
	 * Creates a renderer which uses the window's graphics API and targets the window.
	 *
	 * @return The created Renderer instance.
	 */
	virtual std::shared_ptr<Renderer> make_renderer() = 0;

	/**
	 * Instruct the Window widget to disappear.
	 */
	void close();

protected:
	Window(size_t width, size_t height);

	/**
	 * Determines if the window should be closed.
	 */
	bool should_be_closed = false;

	/**
	 * Current size of the window (in pixels).
	 */
	util::Vector2s size;

	/**
	 * Scaling factor for the window size (also known as "device pixel ratio"
	 * in Qt). Used if OS-level high DPI/fractional scaling is applied.
	 */
	double scale_dpr = 1.0;

	/**
	 * Callbacks for key presses.
	 */
	std::vector<key_cb_t> on_key;

	/**
	 * Callbacks for mouse button presses.
	 */
	std::vector<mouse_button_cb_t> on_mouse_button;

	/**
	 * Callbacks for mouse move actions.
	 */
	std::vector<mouse_move_cb_t> on_mouse_move;

	/**
	 * Callbacks for mouse wheel actions.
	 */
	std::vector<mouse_wheel_cb_t> on_mouse_wheel;

	/**
	 * Callbacks for resize actions.
	 */
	std::vector<resize_cb_t> on_resize;

#if WITH_QT
	/**
	 * Main Qt window handle.
	 */
	std::shared_ptr<QWindow> window;

	/**
	 * Observes and filters events from the Qt window.
	 * Gets attached to window in the window subclasses.
	 */
	std::shared_ptr<WindowEventHandler> event_handler;
#endif
};

} // namespace openage::renderer
