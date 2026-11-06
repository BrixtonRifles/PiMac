// Host keyboard and mouse -> the emulated ADB keyboard and mouse, shared by
// every model. `Adb` is AdbTransceiver (SE, Classic, SE/30) or AdbDevices
// (Egret/Cuda models): both take KeyEvent, MouseMotion and MouseButton.
#pragma once

#include "core/compat.h"

namespace pimac {

// Mac virtual key code -> ADB key code (they differ only for these).
inline u8 AdbFromMacKey(u8 macKey)
{
	switch (macKey) {
	case 0x3B: return 0x36; // Control
	case 0x7B: return 0x3B; // Left
	case 0x7C: return 0x3C; // Right
	case 0x7D: return 0x3D; // Down
	case 0x7E: return 0x3E; // Up
	default: return macKey;
	}
}

template <class Adb>
class AdbInput {
public:
	explicit AdbInput(Adb &adb) : m_Adb(adb), m_CapsLocked(false) { memset(m_Down, 0, sizeof m_Down); }

	// macKey is a Mac virtual key code. Repeats of a held key are dropped;
	// Caps Lock is a locking key: ADB reports it down while locked.
	void KeyEvent(u8 macKey, bool down)
	{
		const u8 code = AdbFromMacKey(macKey);
		if (code >= 0x80)
			return;
		if (code == kCapsLock) {
			if (!down)
				return;
			m_CapsLocked = !m_CapsLocked;
			m_Adb.KeyEvent(code, m_CapsLocked);
			return;
		}
		const u8 bit = (u8)(1u << (code & 7));
		if (down == ((m_Down[code >> 3] & bit) != 0))
			return;
		if (down)
			m_Down[code >> 3] |= bit;
		else
			m_Down[code >> 3] &= (u8)~bit;
		m_Adb.KeyEvent(code, down);
	}

	void MouseMotion(int dx, int dy) { m_Adb.MouseMotion(dx, dy); }
	void MouseButton(bool down) { m_Adb.MouseButton(down); }

	// Releases every held key and the button (focus moved to the host menu).
	void ReleaseAll()
	{
		for (unsigned code = 0; code < 128; ++code)
			if (m_Down[code >> 3] & (1u << (code & 7)))
				m_Adb.KeyEvent((u8)code, false);
		memset(m_Down, 0, sizeof m_Down);
		m_Adb.MouseButton(false);
	}

private:
	static const u8 kCapsLock = 0x39; // ADB code

	Adb &m_Adb;
	bool m_CapsLocked;
	u8 m_Down[16]; // bitmap of ADB codes held down (Caps Lock excluded)
};

} // namespace pimac
