// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

/*
 * Conversion between Qt input events and renderer::WindowEvent (XR fork).
 * Only available in builds with Qt.
 */

#include <memory>

#include <QEvent>

#include "renderer/window_events.h"


namespace openage::renderer {

/**
 * Convert a Qt key, mouse or wheel event.
 *
 * @param event Qt event of type KeyPress, KeyRelease, MouseButtonPress,
 *              MouseButtonRelease, MouseButtonDblClick, MouseMove or Wheel.
 *
 * @return Converted event. Other event types return an event with only the type set.
 */
WindowEvent from_qt_event(const QEvent &event);

/**
 * Create a Qt event from a window event (e.g. to forward it to Qt Quick).
 *
 * @param event Window event.
 *
 * @return Qt event, or \p nullptr if the type is not a key, mouse or wheel event.
 */
std::unique_ptr<QEvent> to_qt_event(const WindowEvent &event);

} // namespace openage::renderer
