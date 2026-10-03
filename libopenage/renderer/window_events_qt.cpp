// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#include "window_events_qt.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QString>
#include <QWheelEvent>

#include "input/keys_qt_check.h"


namespace openage::renderer {

WindowEvent from_qt_event(const QEvent &event) {
	WindowEvent result{};
	result.type = event.type();

	switch (event.type()) {
	case QEvent::KeyPress:
	case QEvent::KeyRelease: {
		auto const &ev = static_cast<const QKeyEvent &>(event);
		result.key = ev.key();
		result.modifiers = ev.modifiers().toInt();
		result.text = ev.text().toStdString();
		result.auto_repeat = ev.isAutoRepeat();
	} break;
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonRelease:
	case QEvent::MouseButtonDblClick:
	case QEvent::MouseMove: {
		auto const &ev = static_cast<const QMouseEvent &>(event);
		result.button = ev.button();
		result.buttons = ev.buttons().toInt();
		result.modifiers = ev.modifiers().toInt();
		result.x = ev.position().x();
		result.y = ev.position().y();
		result.global_x = ev.globalPosition().x();
		result.global_y = ev.globalPosition().y();
	} break;
	case QEvent::Wheel: {
		auto const &ev = static_cast<const QWheelEvent &>(event);
		result.buttons = ev.buttons().toInt();
		result.modifiers = ev.modifiers().toInt();
		result.x = ev.position().x();
		result.y = ev.position().y();
		result.global_x = ev.globalPosition().x();
		result.global_y = ev.globalPosition().y();
		result.angle_delta_x = ev.angleDelta().x();
		result.angle_delta_y = ev.angleDelta().y();
	} break;
	default:
		break;
	}

	return result;
}

std::unique_ptr<QEvent> to_qt_event(const WindowEvent &event) {
	auto type = static_cast<QEvent::Type>(event.type);
	auto modifiers = Qt::KeyboardModifiers::fromInt(event.modifiers);
	auto buttons = Qt::MouseButtons::fromInt(event.buttons);
	QPointF pos{event.x, event.y};
	QPointF global_pos{event.global_x, event.global_y};

	switch (type) {
	case QEvent::KeyPress:
	case QEvent::KeyRelease:
		return std::make_unique<QKeyEvent>(type,
		                                   event.key,
		                                   modifiers,
		                                   QString::fromStdString(event.text),
		                                   event.auto_repeat);
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonRelease:
	case QEvent::MouseButtonDblClick:
	case QEvent::MouseMove:
		return std::make_unique<QMouseEvent>(type,
		                                     pos,
		                                     global_pos,
		                                     static_cast<Qt::MouseButton>(event.button),
		                                     buttons,
		                                     modifiers);
	case QEvent::Wheel:
		return std::make_unique<QWheelEvent>(pos,
		                                     global_pos,
		                                     QPoint{},
		                                     QPoint{event.angle_delta_x, event.angle_delta_y},
		                                     buttons,
		                                     modifiers,
		                                     Qt::NoScrollPhase,
		                                     false);
	default:
		return nullptr;
	}
}

} // namespace openage::renderer
