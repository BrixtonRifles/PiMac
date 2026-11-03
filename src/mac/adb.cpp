#include "mac/adb.h"

namespace pimac {

AdbTransceiver::AdbTransceiver()
{
	m_KeyHead = m_KeyTail = 0;
	m_Dx = m_Dy = 0;
	m_Button = m_ButtonReported = false;
	Reset();
}

void AdbTransceiver::Reset()
{
	m_State = 3;
	m_PendingState = 3;
	m_PendingAt = kNever;
	m_IntHigh = true;
	m_Cmd = 0;
	m_Listen = false;
	m_TalkData = false;
	m_Size = m_Index = 0;
	m_KeyboardAddress = 2;
	m_MouseAddress = 3;
	m_RandomAddress = 1;
}

void AdbTransceiver::StateLinesChanged(unsigned state, u64 now)
{
	if (state == m_PendingState && m_PendingAt != kNever)
		return;
	if (state == m_State && m_PendingAt == kNever)
		return;
	m_PendingState = state;
	m_PendingAt = now + kResponseCycles;
}

void AdbTransceiver::Update(u64 now, Via6522 &via)
{
	if (m_PendingAt == kNever || now < m_PendingAt)
		return;
	m_PendingAt = kNever;
	m_State = m_PendingState;
	DoNewState(via);
}

void AdbTransceiver::DoNewState(Via6522 &via)
{
	m_IntHigh = true;

	switch (m_State) {
	case 0: // start a new command (the Mac has shifted it out)
		if (m_Listen) {
			m_Listen = false;
			m_Size = m_Index;
			EndListen();
		}
		m_TalkData = false;
		m_Index = 0;
		m_Size = 0;
		m_Cmd = via.DeviceShiftOut();
		DoCommand();
		break;

	case 1: // transfer data byte (even)
	case 2: // transfer data byte (odd)
		if (!m_Listen) {
			if (!m_TalkData || m_Index >= m_Size) {
				via.DeviceShiftIn(0xFF);
				m_IntHigh = false; // no (more) data: timeout
			} else {
				via.DeviceShiftIn(m_Buf[m_Index++]);
			}
		} else {
			u8 b = via.DeviceShiftOut();
			if (m_Index < sizeof m_Buf)
				m_Buf[m_Index++] = b;
		}
		break;

	case 3: // idle
	default:
		// Pending input is reported by Poll() at the next VBL, like the real
		// transceiver's ~11 ms auto-poll. Answering at once can land while the
		// ROM is still finishing ADB initialisation, and it drops the request.
		if (m_TalkData)
			via.DeviceShiftIn(0xFF);
		break;
	}
}

void AdbTransceiver::Poll(Via6522 &via)
{
	if (m_State != 3 || m_PendingAt != kNever || m_TalkData)
		return;
	if (!AnyEvent())
		return;
	if (((m_Cmd >> 2) & 3) == 3)
		Talk();
	via.DeviceShiftIn(0xFF);
	m_IntHigh = false;
}

void AdbTransceiver::DoCommand()
{
	switch ((m_Cmd >> 2) & 3) {
	case 0:
		if ((m_Cmd & 0x0F) == 0x00) { // SendReset
			m_KeyboardAddress = 2;
			m_MouseAddress = 3;
		} else if ((m_Cmd & 0x0F) == 0x01) { // Flush
			u8 address = m_Cmd >> 4;
			if (address == m_KeyboardAddress || address == m_MouseAddress) {
				m_Buf[0] = m_Buf[1] = 0;
				m_Size = 2;
				m_TalkData = true;
			}
		}
		break;
	case 2: // Listen
		m_Listen = true;
		break;
	case 3: // Talk
		Talk();
		break;
	default:
		break;
	}
}

void AdbTransceiver::Talk()
{
	m_TalkData = false;
	m_Size = 0;
	m_Index = 0;
	u8 address = m_Cmd >> 4;
	unsigned reg = m_Cmd & 3;
	if (address == m_MouseAddress)
		TalkMouse(reg);
	else if (address == m_KeyboardAddress)
		TalkKeyboard(reg);
}

void AdbTransceiver::TalkRegister3()
{
	m_Buf[0] = (u8)(0x60 | (m_RandomAddress & 0x0F));
	m_Buf[1] = 0x01;
	m_Size = 2;
	m_TalkData = true;
	++m_RandomAddress;
}

void AdbTransceiver::TalkMouse(unsigned reg)
{
	if (reg == 3) {
		TalkRegister3();
		return;
	}
	if (reg != 0)
		return;
	if (m_Dx == 0 && m_Dy == 0 && m_Button == m_ButtonReported)
		return;

	int dx = m_Dx < -63 ? -63 : m_Dx > 63 ? 63 : m_Dx;
	int dy = m_Dy < -63 ? -63 : m_Dy > 63 ? 63 : m_Dy;
	m_Dx -= dx;
	m_Dy -= dy;
	m_ButtonReported = m_Button;

	m_Buf[0] = (u8)((m_Button ? 0x00 : 0x80) | (dy & 0x7F));
	m_Buf[1] = (u8)(0x80 | (dx & 0x7F));
	m_Size = 2;
	m_TalkData = true;
}

void AdbTransceiver::TalkKeyboard(unsigned reg)
{
	if (reg == 3) {
		TalkRegister3();
		return;
	}
	if (reg != 0)
		return;
	u8 first, second;
	if (!PopKey(first))
		return;
	if (!PopKey(second))
		second = 0xFF;
	m_Buf[0] = first;
	m_Buf[1] = second;
	m_Size = 2;
	m_TalkData = true;
}

void AdbTransceiver::EndListen()
{
	u8 address = m_Cmd >> 4;
	if ((m_Cmd & 3) != 3 || m_Size < 2 || m_Buf[1] != 0xFE)
		return;
	// Listen register 3 with handler 0xFE: change device address.
	if (address == m_MouseAddress)
		m_MouseAddress = m_Buf[0] & 0x0F;
	else if (address == m_KeyboardAddress)
		m_KeyboardAddress = m_Buf[0] & 0x0F;
}

bool AdbTransceiver::AnyEvent() const
{
	return m_KeyHead != m_KeyTail || m_Dx != 0 || m_Dy != 0 || m_Button != m_ButtonReported;
}

bool AdbTransceiver::PopKey(u8 &event)
{
	if (m_KeyHead == m_KeyTail)
		return false;
	event = m_Keys[m_KeyTail];
	m_KeyTail = (m_KeyTail + 1) % kKeyQueue;
	return true;
}

void AdbTransceiver::KeyEvent(u8 adbCode, bool down)
{
	unsigned next = (m_KeyHead + 1) % kKeyQueue;
	if (next == m_KeyTail)
		return; // full
	m_Keys[m_KeyHead] = (u8)((adbCode & 0x7F) | (down ? 0x00 : 0x80));
	m_KeyHead = next;
}

void AdbTransceiver::MouseMotion(int dx, int dy)
{
	m_Dx += dx;
	m_Dy += dy;
	// Don't let a burst of movement build up an unbounded backlog.
	if (m_Dx > 1000) m_Dx = 1000;
	if (m_Dx < -1000) m_Dx = -1000;
	if (m_Dy > 1000) m_Dy = 1000;
	if (m_Dy < -1000) m_Dy = -1000;
}

void AdbTransceiver::MouseButton(bool down)
{
	m_Button = down;
}

} // namespace pimac
