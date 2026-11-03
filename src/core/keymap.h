// USB HID keyboard usages -> classic Macintosh virtual key codes.
//
// The Mac codes are the documented "virtual key codes" (Inside Macintosh /
// Events.h kVK_*), which match ADB key codes for all keys except the right-hand
// modifiers. Each emulation core translates these into what its keyboard
// hardware expects (ADB for the Classic/SE, M0110 for the 128K/512K/Plus).
#pragma once

#include "core/compat.h"

namespace pimac {

enum : u8 { kMacKeyNone = 0xFF };

namespace hid {
enum : u8 {
	Enter = 0x28,
	Escape = 0x29,
	Space = 0x2C,
	F12 = 0x45,
	Right = 0x4F,
	Left = 0x50,
	Down = 0x51,
	Up = 0x52,
	KeypadEnter = 0x58,
	// Modifier pseudo-usages (as defined by the HID usage tables)
	LeftCtrl = 0xE0,
	LeftShift = 0xE1,
	LeftAlt = 0xE2,
	LeftGUI = 0xE3,
	RightCtrl = 0xE4,
	RightShift = 0xE5,
	RightAlt = 0xE6,
	RightGUI = 0xE7,
};
} // namespace hid

// altIsCommand: true maps the PC Alt key (next to the space bar) to Command
// and the Windows key to Option, matching the physical Mac layout.
u8 MacKeyFromHID(u8 usage, bool altIsCommand);

} // namespace pimac
