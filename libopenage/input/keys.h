// Copyright 2026-2026 the openage authors. See copying.md for legal info.

#pragma once

/*
 * Input constants without a Qt dependency (XR fork).
 *
 * The numeric values are the same as those of Qt (QEvent::Type, Qt::Key,
 * Qt::MouseButton and Qt::KeyboardModifier), so bindings, configuration and
 * events created by the Qt window stay bit-identical. In builds with Qt,
 * input/keys_qt_check.h compares every constant against Qt at compile time.
 */

namespace openage::input {

/**
 * Event types (values of QEvent::Type).
 */
namespace event_type {
enum : int {
	NoEvent = 0,
	MouseButtonPress = 2,
	MouseButtonRelease = 3,
	MouseButtonDblClick = 4,
	MouseMove = 5,
	KeyPress = 6,
	KeyRelease = 7,
	Wheel = 31,
};
} // namespace event_type

/**
 * Mouse buttons (values of Qt::MouseButton).
 */
namespace mouse_button {
enum : int {
	NoButton = 0x00000000,
	LeftButton = 0x00000001,
	RightButton = 0x00000002,
	MiddleButton = 0x00000004,
	BackButton = 0x00000008,
	ForwardButton = 0x00000010,
};
} // namespace mouse_button

/**
 * Keyboard modifiers (values of Qt::KeyboardModifier).
 */
namespace modifier {
enum : int {
	NoModifier = 0x00000000,
	ShiftModifier = 0x02000000,
	ControlModifier = 0x04000000,
	AltModifier = 0x08000000,
	MetaModifier = 0x10000000,
	KeypadModifier = 0x20000000,
	GroupSwitchModifier = 0x40000000,
};
} // namespace modifier

/**
 * All named keys as X-macro: X(name, value) with the values of Qt::Key.
 *
 * Printable characters outside this list (e.g. Latin-1 letters) use their
 * upper case Unicode code point as key code, like Qt.
 */
#define OPENAGE_INPUT_KEYS(X)                    \
	X(Key_Space, 0x20)                           \
	X(Key_Exclam, 0x21)                          \
	X(Key_QuoteDbl, 0x22)                        \
	X(Key_NumberSign, 0x23)                      \
	X(Key_Dollar, 0x24)                          \
	X(Key_Percent, 0x25)                         \
	X(Key_Ampersand, 0x26)                       \
	X(Key_Apostrophe, 0x27)                      \
	X(Key_ParenLeft, 0x28)                       \
	X(Key_ParenRight, 0x29)                      \
	X(Key_Asterisk, 0x2a)                        \
	X(Key_Plus, 0x2b)                            \
	X(Key_Comma, 0x2c)                           \
	X(Key_Minus, 0x2d)                           \
	X(Key_Period, 0x2e)                          \
	X(Key_Slash, 0x2f)                           \
	X(Key_0, 0x30)                               \
	X(Key_1, 0x31)                               \
	X(Key_2, 0x32)                               \
	X(Key_3, 0x33)                               \
	X(Key_4, 0x34)                               \
	X(Key_5, 0x35)                               \
	X(Key_6, 0x36)                               \
	X(Key_7, 0x37)                               \
	X(Key_8, 0x38)                               \
	X(Key_9, 0x39)                               \
	X(Key_Colon, 0x3a)                           \
	X(Key_Semicolon, 0x3b)                       \
	X(Key_Less, 0x3c)                            \
	X(Key_Equal, 0x3d)                           \
	X(Key_Greater, 0x3e)                         \
	X(Key_Question, 0x3f)                        \
	X(Key_At, 0x40)                              \
	X(Key_A, 0x41)                               \
	X(Key_B, 0x42)                               \
	X(Key_C, 0x43)                               \
	X(Key_D, 0x44)                               \
	X(Key_E, 0x45)                               \
	X(Key_F, 0x46)                               \
	X(Key_G, 0x47)                               \
	X(Key_H, 0x48)                               \
	X(Key_I, 0x49)                               \
	X(Key_J, 0x4a)                               \
	X(Key_K, 0x4b)                               \
	X(Key_L, 0x4c)                               \
	X(Key_M, 0x4d)                               \
	X(Key_N, 0x4e)                               \
	X(Key_O, 0x4f)                               \
	X(Key_P, 0x50)                               \
	X(Key_Q, 0x51)                               \
	X(Key_R, 0x52)                               \
	X(Key_S, 0x53)                               \
	X(Key_T, 0x54)                               \
	X(Key_U, 0x55)                               \
	X(Key_V, 0x56)                               \
	X(Key_W, 0x57)                               \
	X(Key_X, 0x58)                               \
	X(Key_Y, 0x59)                               \
	X(Key_Z, 0x5a)                               \
	X(Key_BracketLeft, 0x5b)                     \
	X(Key_Backslash, 0x5c)                       \
	X(Key_BracketRight, 0x5d)                    \
	X(Key_AsciiCircum, 0x5e)                     \
	X(Key_Underscore, 0x5f)                      \
	X(Key_QuoteLeft, 0x60)                       \
	X(Key_BraceLeft, 0x7b)                       \
	X(Key_Bar, 0x7c)                             \
	X(Key_BraceRight, 0x7d)                      \
	X(Key_AsciiTilde, 0x7e)                      \
	X(Key_Escape, 0x01000000)                    \
	X(Key_Tab, 0x01000001)                       \
	X(Key_Backtab, 0x01000002)                   \
	X(Key_Backspace, 0x01000003)                 \
	X(Key_Return, 0x01000004)                    \
	X(Key_Enter, 0x01000005)                     \
	X(Key_Insert, 0x01000006)                    \
	X(Key_Delete, 0x01000007)                    \
	X(Key_Pause, 0x01000008)                     \
	X(Key_Print, 0x01000009)                     \
	X(Key_SysReq, 0x0100000a)                    \
	X(Key_Clear, 0x0100000b)                     \
	X(Key_Home, 0x01000010)                      \
	X(Key_End, 0x01000011)                       \
	X(Key_Left, 0x01000012)                      \
	X(Key_Up, 0x01000013)                        \
	X(Key_Right, 0x01000014)                     \
	X(Key_Down, 0x01000015)                      \
	X(Key_PageUp, 0x01000016)                    \
	X(Key_PageDown, 0x01000017)                  \
	X(Key_Shift, 0x01000020)                     \
	X(Key_Control, 0x01000021)                   \
	X(Key_Meta, 0x01000022)                      \
	X(Key_Alt, 0x01000023)                       \
	X(Key_CapsLock, 0x01000024)                  \
	X(Key_NumLock, 0x01000025)                   \
	X(Key_ScrollLock, 0x01000026)                \
	X(Key_F1, 0x01000030)                        \
	X(Key_F2, 0x01000031)                        \
	X(Key_F3, 0x01000032)                        \
	X(Key_F4, 0x01000033)                        \
	X(Key_F5, 0x01000034)                        \
	X(Key_F6, 0x01000035)                        \
	X(Key_F7, 0x01000036)                        \
	X(Key_F8, 0x01000037)                        \
	X(Key_F9, 0x01000038)                        \
	X(Key_F10, 0x01000039)                       \
	X(Key_F11, 0x0100003a)                       \
	X(Key_F12, 0x0100003b)                       \
	X(Key_F13, 0x0100003c)                       \
	X(Key_F14, 0x0100003d)                       \
	X(Key_F15, 0x0100003e)                       \
	X(Key_F16, 0x0100003f)                       \
	X(Key_F17, 0x01000040)                       \
	X(Key_F18, 0x01000041)                       \
	X(Key_F19, 0x01000042)                       \
	X(Key_F20, 0x01000043)                       \
	X(Key_F21, 0x01000044)                       \
	X(Key_F22, 0x01000045)                       \
	X(Key_F23, 0x01000046)                       \
	X(Key_F24, 0x01000047)                       \
	X(Key_F25, 0x01000048)                       \
	X(Key_F26, 0x01000049)                       \
	X(Key_F27, 0x0100004a)                       \
	X(Key_F28, 0x0100004b)                       \
	X(Key_F29, 0x0100004c)                       \
	X(Key_F30, 0x0100004d)                       \
	X(Key_F31, 0x0100004e)                       \
	X(Key_F32, 0x0100004f)                       \
	X(Key_F33, 0x01000050)                       \
	X(Key_F34, 0x01000051)                       \
	X(Key_F35, 0x01000052)                       \
	X(Key_Super_L, 0x01000053)                   \
	X(Key_Super_R, 0x01000054)                   \
	X(Key_Menu, 0x01000055)                      \
	X(Key_Hyper_L, 0x01000056)                   \
	X(Key_Hyper_R, 0x01000057)                   \
	X(Key_Help, 0x01000058)                      \
	X(Key_unknown, 0x01ffffff)

/**
 * Key codes (values of Qt::Key).
 */
namespace key {
enum : int {
#define OPENAGE_INPUT_KEY_ENUM(name, value) name = value,
	OPENAGE_INPUT_KEYS(OPENAGE_INPUT_KEY_ENUM)
#undef OPENAGE_INPUT_KEY_ENUM
};
} // namespace key

} // namespace openage::input
