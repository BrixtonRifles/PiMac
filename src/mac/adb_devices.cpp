#include "mac/adb_devices.h"

namespace pimac {

AdbDevices::AdbDevices()
{
	m_KeyHead = m_KeyTail = 0;
	memset(m_Down, 0, sizeof m_Down);
	m_Dx = m_Dy = 0;
	m_Button = m_ButtonReported = false;
	m_Random = 1;
	Reset();
}

void AdbDevices::Reset()
{
	m_KeyboardAddress = 2;
	m_MouseAddress = 3;
	m_KeyboardHandler = 2; // extended keyboard
	m_MouseHandler = 1;    // 100 cpi mouse
}

int AdbDevices::Command(u8 cmd, const u8 *data, unsigned len, u8 *reply)
{
	const u8 address = cmd >> 4;
	const unsigned reg = cmd & 3;
	switch ((cmd >> 2) & 3) {
	case 0:
		if ((cmd & 0x0F) == 0x00) // SendReset
			Reset();
		// Flush: nothing buffered beyond the key queue; keep it.
		return 0;
	case 2: // Listen
		if (reg == 3 && len >= 2) {
			// Register 3: new address (handler 0xFE), or a handler ID.
			u8 *handler = address == m_KeyboardAddress ? &m_KeyboardHandler
				      : address == m_MouseAddress  ? &m_MouseHandler
								   : 0;
			if (!handler)
				return 0;
			if (data[1] == 0xFE) {
				if (address == m_KeyboardAddress)
					m_KeyboardAddress = data[0] & 0x0F;
				else
					m_MouseAddress = data[0] & 0x0F;
			} else if (data[1] != 0x00 && data[1] != 0xFD && data[1] != 0xFF) {
				*handler = data[1];
			}
		}
		return 0;
	case 3: // Talk
		if (address == m_MouseAddress)
			return TalkMouse(reg, reply);
		if (address == m_KeyboardAddress)
			return TalkKeyboard(reg, reply);
		return -1;
	default:
		return -1;
	}
}

bool AdbDevices::MousePending() const
{
	return m_Dx != 0 || m_Dy != 0 || m_Button != m_ButtonReported;
}

bool AdbDevices::HasData(u8 address) const
{
	if (address == m_KeyboardAddress)
		return KeysPending();
	if (address == m_MouseAddress)
		return MousePending();
	return false;
}

int AdbDevices::TalkRegister3(u8 address, u8 *reply)
{
	// Address with a random bit pattern in the high nibble (collision
	// detection), and the handler ID.
	reply[0] = (u8)(0x60 | (address & 0x0F));
	(void)m_Random;
	reply[1] = address == m_KeyboardAddress ? m_KeyboardHandler : m_MouseHandler;
	return 2;
}

int AdbDevices::TalkMouse(unsigned reg, u8 *reply)
{
	if (reg == 3)
		return TalkRegister3(m_MouseAddress, reply);
	if (reg != 0 || !MousePending())
		return -1;
	const int dx = m_Dx < -63 ? -63 : m_Dx > 63 ? 63 : m_Dx;
	const int dy = m_Dy < -63 ? -63 : m_Dy > 63 ? 63 : m_Dy;
	m_Dx -= dx;
	m_Dy -= dy;
	m_ButtonReported = m_Button;
	reply[0] = (u8)((m_Button ? 0x00 : 0x80) | (dy & 0x7F));
	reply[1] = (u8)(0x80 | (dx & 0x7F));
	return 2;
}

int AdbDevices::TalkKeyboard(unsigned reg, u8 *reply)
{
	if (reg == 3)
		return TalkRegister3(m_KeyboardAddress, reply);
	if (reg == 2) {
		// Modifier state: all released except what is held (active low).
		u8 hi = 0xFF, lo = 0xFF;
		struct Bit { u8 code; u8 *byte; u8 mask; };
		const Bit bits[] = {
			{0x37, &hi, 0x01}, {0x3A, &hi, 0x02}, {0x38, &hi, 0x04},
			{0x36, &hi, 0x08}, {0x39, &hi, 0x20},
		};
		for (unsigned i = 0; i < sizeof bits / sizeof bits[0]; ++i)
			if (m_Down[bits[i].code >> 3] & (1u << (bits[i].code & 7)))
				*bits[i].byte &= (u8)~bits[i].mask;
		reply[0] = hi;
		reply[1] = lo;
		return 2;
	}
	if (reg != 0)
		return -1;
	u8 first, second;
	if (!PopKey(first))
		return -1;
	if (!PopKey(second))
		second = 0xFF;
	reply[0] = first;
	reply[1] = second;
	return 2;
}

bool AdbDevices::PopKey(u8 &event)
{
	if (m_KeyHead == m_KeyTail)
		return false;
	event = m_Keys[m_KeyTail];
	m_KeyTail = (m_KeyTail + 1) % kKeyQueue;
	return true;
}

void AdbDevices::KeyEvent(u8 adbCode, bool down)
{
	adbCode &= 0x7F;
	if (down)
		m_Down[adbCode >> 3] |= (u8)(1u << (adbCode & 7));
	else
		m_Down[adbCode >> 3] &= (u8)~(1u << (adbCode & 7));
	const unsigned next = (m_KeyHead + 1) % kKeyQueue;
	if (next == m_KeyTail)
		return; // full
	m_Keys[m_KeyHead] = (u8)(adbCode | (down ? 0x00 : 0x80));
	m_KeyHead = next;
}

void AdbDevices::ReleaseAll()
{
	for (unsigned code = 0; code < 128; ++code)
		if (m_Down[code >> 3] & (1u << (code & 7)))
			KeyEvent((u8)code, false);
	MouseButton(false);
}

void AdbDevices::MouseMotion(int dx, int dy)
{
	m_Dx += dx;
	m_Dy += dy;
	if (m_Dx > 1000) m_Dx = 1000;
	if (m_Dx < -1000) m_Dx = -1000;
	if (m_Dy > 1000) m_Dy = 1000;
	if (m_Dy < -1000) m_Dy = -1000;
}

void AdbDevices::MouseButton(bool down)
{
	m_Button = down;
}

} // namespace pimac
