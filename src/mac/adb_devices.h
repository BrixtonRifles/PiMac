// The emulated ADB keyboard (address 2) and mouse (address 3), answering
// whole ADB commands. Used by the Egret (Classic II); the SE/Classic VIA
// transceiver (adb.cpp) has its own byte-level copy of the same behaviour.
#pragma once

#include "core/compat.h"

namespace pimac {

class AdbDevices {
public:
	AdbDevices();

	void Reset(); // ADB SendReset: default addresses

	// One ADB command. Listen data in `data`/`len`; Talk data is written to
	// `reply`. Returns the reply length, or -1 if no device answered (a
	// Talk to an empty address, or a Talk with no new data).
	int Command(u8 cmd, const u8 *data, unsigned len, u8 *reply);

	// True if Talk register 0 of `address` would return data.
	bool HasData(u8 address) const;
	u8 KeyboardAddress() const { return m_KeyboardAddress; }
	u8 MouseAddress() const { return m_MouseAddress; }

	// Host input. Key codes are ADB key codes; bit 7 is never set here.
	void KeyEvent(u8 adbCode, bool down);
	void MouseMotion(int dx, int dy);
	void MouseButton(bool down);
	void ReleaseAll(); // release any keys held

private:
	int TalkMouse(unsigned reg, u8 *reply);
	int TalkKeyboard(unsigned reg, u8 *reply);
	int TalkRegister3(u8 address, u8 *reply);
	bool KeysPending() const { return m_KeyHead != m_KeyTail; }
	bool MousePending() const;
	bool PopKey(u8 &event);

	u8 m_KeyboardAddress, m_MouseAddress;
	u8 m_KeyboardHandler, m_MouseHandler;
	u8 m_Random;

	enum { kKeyQueue = 64 };
	u8 m_Keys[kKeyQueue];
	unsigned m_KeyHead, m_KeyTail;
	u8 m_Down[16]; // bitmap of ADB codes held down
	int m_Dx, m_Dy;
	bool m_Button, m_ButtonReported;
};

} // namespace pimac
