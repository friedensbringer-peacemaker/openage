// Copyright 2015-2026 the openage authors. See copying.md for legal info.

#include "window.h"

#include "error/error.h"

#if WITH_QT
	#include <QWindow>

	#include "opengl/window.h"
	#include "window_event_handler.h"
#endif


namespace openage::renderer {

std::shared_ptr<Window> Window::create(const std::string &title,
                                       window_settings settings) {
#if WITH_QT
	// currently we only have a functional GL window
	// TODO: support other renderer windows
	//       and add some selection mechanism.
	return std::make_shared<opengl::GlWindow>(title, settings);
#else
	// XR fork: builds without Qt get their window from the embedder (EGL sink)
	(void)settings;
	throw Error{MSG(err) << "Cannot create window '" << title
	                     << "': no window backend in this build (built with OPENAGE_QT=OFF)."};
#endif
}


Window::Window(size_t width, size_t height) :
	size{width, height}
#if WITH_QT
	,
	event_handler{std::make_shared<WindowEventHandler>()}
#endif
{
}


const util::Vector2s &Window::get_size() const {
	return this->size;
}

double Window::get_scale() const {
	return this->scale_dpr;
}

bool Window::should_close() const {
	return this->should_be_closed;
}

void Window::add_mouse_button_callback(const mouse_button_cb_t &cb) {
	this->on_mouse_button.push_back(cb);
}

void Window::add_mouse_move_callback(const mouse_move_cb_t &cb) {
	this->on_mouse_move.push_back(cb);
}

void Window::add_mouse_wheel_callback(const mouse_wheel_cb_t &cb) {
	this->on_mouse_wheel.push_back(cb);
}

void Window::add_key_callback(const key_cb_t &cb) {
	this->on_key.push_back(cb);
}

void Window::add_resize_callback(const resize_cb_t &cb) {
	this->on_resize.push_back(cb);
}

#if WITH_QT
const std::shared_ptr<QWindow> &Window::get_qt_window() const {
	return this->window;
}
#endif

void Window::close() {
#if WITH_QT
	this->window->close();
#else
	this->should_be_closed = true;
#endif
}


} // namespace openage::renderer
