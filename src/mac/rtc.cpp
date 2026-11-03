#include "mac/rtc.h"

namespace pimac {

Rtc::Rtc() : m_Seconds(0), m_PramDirty(false), m_WriteProtect(false)
{
	SetDefaultPram();
	Reset();
}

void Rtc::SetDefaultPram()
{
	// What the Classic ROM writes to blank PRAM, except SPConfig (0x13) =
	// 0x22: both serial ports in use for async, so AppleTalk stays inactive.
	// With AppleTalk on, the LocalTalk driver waits forever for SCC
	// interrupts that this emulator does not generate.
	static const u8 kDefault[32] = {
		0x00, 0x00, 0x4F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x88, 0x00, 0x4C, 0x42, 0x75, 0x67, 0x73,
		0xA8, 0x00, 0x00, 0x22, 0xCC, 0x0A, 0xCC, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x63, 0x00,
	};
	memset(m_Pram, 0, sizeof m_Pram);
	memcpy(m_Pram, kDefault, sizeof kDefault);
}

void Rtc::Reset()
{
	m_Enabled = false;
	m_Clock = false;
	m_Mode = ModeCommand;
	m_Shift = 0;
	m_Bits = 0;
	m_Cmd = 0;
	m_ExtAddress = -1;
	m_OutBit = 1;
}

void Rtc::PinsChanged(u8 pins, bool macDrivesData)
{
	const bool enabled = (pins & 0x04) == 0; // active low
	const bool clock = (pins & 0x02) != 0;

	if (!enabled) {
		m_Enabled = false;
		m_Clock = clock;
		m_Mode = ModeCommand;
		m_Bits = 0;
		return;
	}
	if (!m_Enabled) {
		m_Enabled = true;
		m_Mode = ModeCommand;
		m_Bits = 0;
	}

	const bool rising = clock && !m_Clock;
	m_Clock = clock;
	if (!rising)
		return;

	switch (m_Mode) {
	case ModeCommand:
	case ModeCommand2:
	case ModeWrite:
		if (!macDrivesData)
			break;
		m_Shift = (u8)((m_Shift << 1) | (pins & 1));
		if (++m_Bits == 8) {
			m_Bits = 0;
			ByteReceived(m_Shift);
		}
		break;
	case ModeRead:
		// Present the next bit (MSB first); the Mac samples after the rising edge.
		m_OutBit = (m_Shift >> 7) & 1;
		m_Shift = (u8)(m_Shift << 1);
		if (++m_Bits == 8)
			m_Mode = ModeDone;
		break;
	case ModeDone:
		break;
	}
}

int Rtc::PramAddress(u8 cmd) const
{
	const u8 c = cmd & 0x7F;
	if ((c & 0x78) == 0x38)
		return m_ExtAddress;           // extended: z0111abc 0defgh00
	if (c & 0x40)
		return 0x10 + ((c >> 2) & 0x0F); // z1aaaa01
	if ((c & 0x70) == 0x20)
		return 0x08 + ((c >> 2) & 0x03); // z010aa01
	return -1;
}

void Rtc::ByteReceived(u8 byte)
{
	if (m_Mode == ModeCommand) {
		m_Cmd = byte;
		if ((byte & 0x78) == 0x38) {
			m_Mode = ModeCommand2;
			return;
		}
	} else if (m_Mode == ModeCommand2) {
		m_ExtAddress = ((m_Cmd & 0x07) << 5) | ((byte >> 2) & 0x1F);
	} else if (m_Mode == ModeWrite) {
		WriteRegister(m_Cmd, byte);
		m_Mode = ModeDone;
		return;
	}

	// Command complete: read or write?
	if (m_Cmd & 0x80) {
		m_Shift = ReadRegister(m_Cmd);
		m_Bits = 0;
		m_Mode = ModeRead;
	} else {
		m_Mode = ModeWrite;
	}
}

u8 Rtc::ReadRegister(u8 cmd) const
{
	int addr = PramAddress(cmd);
	if (addr >= 0)
		return m_Pram[addr & 0xFF];
	const u8 c = cmd & 0x7F;
	if ((c & 0x60) == 0x00) // seconds: z00xaa01 (x ignored)
		return (u8)(m_Seconds >> (8 * ((c >> 2) & 3)));
	return 0;
}

void Rtc::WriteRegister(u8 cmd, u8 value)
{
	const u8 c = cmd & 0x7F;
	if (c == 0x35) { // write-protect register
		m_WriteProtect = (value & 0x80) != 0;
		return;
	}
	if (c == 0x31) // test register
		return;
	if (m_WriteProtect)
		return;

	int addr = PramAddress(cmd);
	if (addr >= 0) {
		if (m_Pram[addr & 0xFF] != value) {
			m_Pram[addr & 0xFF] = value;
			m_PramDirty = true;
		}
		return;
	}
	if ((c & 0x60) == 0x00) { // seconds: z00xaa01 (x ignored)
		unsigned shift = 8 * ((c >> 2) & 3);
		m_Seconds = (m_Seconds & ~(0xFFu << shift)) | ((u32)value << shift);
	}
}

} // namespace pimac
