#include "mac/asc.h"


namespace pimac {

AscV8::AscV8()
{
	Reset();
}

void AscV8::Reset()
{
	memset(m_Fifo, 0x80, sizeof m_Fifo);
	memset(m_Regs, 0, sizeof m_Regs);
	m_Read = m_Count = 0;
	m_Volume = 0;
	m_Irq = false;
	m_Last = 0;
	m_Status = 0x02; // FIFO A empty
}

void AscV8::UpdateStatus()
{
	if (m_Count < 0x200) {
		m_Status |= 0x01;
		m_Irq = true;
	} else {
		m_Status &= (u8)~0x01;
	}
	if (m_Count == 0 || m_Count == sizeof m_Fifo) // bit 1: empty or full
		m_Status |= 0x02;
	else
		m_Status &= (u8)~0x02;
}

u8 AscV8::Read(u32 offset)
{
	offset &= 0xFFF;
	if (offset < 0x800)
		return 0;
	switch (offset) {
	case 0x800: return 0xE8; // version: V8
	case 0x801: // mode, control and FIFO mode read as 1 on V8
	case 0x802:
	case 0x803:
		return 1;
	case 0x804: {
		const u8 v = m_Status;
		if (!(m_Status & 0x01))
			m_Irq = false; // reading clears the interrupt, not the status
		return v;
	}
	case 0x805: case 0x807: case 0x80F:
		return 0;
	case 0x806:
		return m_Volume;
	case 0xF09: case 0xF29:
		return 1;
	default:
		return offset < 0x840 ? m_Regs[offset - 0x800] : 0;
	}
}

void AscV8::Write(u32 offset, u8 value)
{
	offset &= 0xFFF;
	if (offset < 0x400) { // FIFO A
		if (m_Count < sizeof m_Fifo) {
			m_Fifo[(m_Read + m_Count) % sizeof m_Fifo] = value;
			++m_Count;
		}
		UpdateStatus();
		if (!(m_Status & 0x01))
			m_Irq = false;
		return;
	}
	if (offset < 0x800)
		return; // FIFO B does not exist on V8
	switch (offset) {
	case 0x801: case 0x802: case 0x805: case 0x807:
		return; // read-only / nothing on V8
	case 0x803:
		if (value & 0x80) { // clear FIFO
			m_Read = m_Count = 0;
			UpdateStatus();
		}
		break;
	case 0x806:
		m_Volume = value;
		break;
	default:
		break;
	}
	if (offset < 0x840)
		m_Regs[offset - 0x800] = value;
}

s16 AscV8::NextSample()
{
	if (m_Count) {
		const s8 sample = (s8)(m_Fifo[m_Read] ^ 0x80);
		m_Read = (m_Read + 1) % sizeof m_Fifo;
		--m_Count;
		const unsigned volume = (m_Volume >> 5) & 7;
		m_Last = (s16)(sample * 256 * (int)volume / 7); // as the Classic: full scale at 7
		UpdateStatus();
	} else {
		UpdateStatus(); // stays "empty": keep asking
	}
	return m_Last;
}

} // namespace pimac
