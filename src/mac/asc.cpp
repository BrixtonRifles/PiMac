#include "mac/asc.h"

namespace pimac {

bool AscV8::Fifo::Push(u8 v)
{
	if (count >= sizeof data)
		return false;
	data[(read + count) % sizeof data] = v;
	++count;
	return true;
}

bool AscV8::Fifo::Pop(u8 &v)
{
	if (!count)
		return false;
	v = data[read];
	read = (read + 1) % sizeof data;
	--count;
	return true;
}

AscV8::AscV8(bool sonora) : m_Sonora(sonora)
{
	Reset();
}

void AscV8::Reset()
{
	memset(m_A.data, 0x80, sizeof m_A.data);
	memset(m_B.data, 0x80, sizeof m_B.data);
	m_A.read = m_A.count = m_B.read = m_B.count = 0;
	memset(m_Regs, 0, sizeof m_Regs);
	m_Volume = 0;
	m_IrqDisable[0] = m_IrqDisable[1] = 0; // Sonora: enabled at reset (MAME)
	m_Irq = false;
	m_Last = 0;
	m_LastA = m_LastB = 0;
	m_Status = 0x02; // FIFO A empty
}

void AscV8::UpdateStatus()
{
	if (!m_Sonora) {
		if (m_A.count < 0x200) {
			m_Status |= 0x01;
			m_Irq = true;
		} else {
			m_Status &= (u8)~0x01;
		}
		if (m_A.count == 0 || m_A.count == sizeof m_A.data) // bit 1: empty or full
			m_Status |= 0x02;
		else
			m_Status &= (u8)~0x02;
		return;
	}
	// Sonora, after each output sample (MAME's sound_stream_update): FIFO A
	// always reads empty in playback mode and then interrupts if enabled;
	// bit 2 = either FIFO half empty, bit 3 = either FIFO empty. (Writes set
	// bit 3 when B fills; see Write.)
	m_Status |= 0x02;
	if (!(m_IrqDisable[0] & 1))
		m_Irq = true;
	if (m_A.count < 0x200 || m_B.count < 0x200) {
		m_Status |= 0x04;
		if (!(m_IrqDisable[1] & 1))
			m_Irq = true;
	} else {
		m_Status &= (u8)~0x04;
	}
	if (m_A.count == 0 || m_B.count == 0)
		m_Status |= 0x08;
	else
		m_Status &= (u8)~0x08;
}

u8 AscV8::Read(u32 offset)
{
	offset &= 0xFFF;
	if (offset < 0x800)
		return 0;
	if (m_Sonora) {
		switch (offset) {
		case 0x800: return 0xBC; // version: Sonora
		case 0x801: return 1;
		case 0x802: case 0x803: case 0x805: case 0x807: case 0x80F:
			return 0;
		case 0x804:
			if (!(m_Status & 0x04))
				m_Irq = false; // reading clears the interrupt, not the status
			return m_Status;
		case 0x806:
			return m_Volume;
		case 0xF09: return m_IrqDisable[0];
		case 0xF29: return m_IrqDisable[1];
		default:
			return offset < 0x840 ? m_Regs[offset - 0x800] : 0;
		}
	}
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
		m_A.Push(value);
		if (!m_Sonora) {
			UpdateStatus();
			if (!(m_Status & 0x01))
				m_Irq = false;
			return;
		}
		if (m_A.count >= 0x200)
			m_Status &= (u8)~0x01;
		m_Status |= 0x02; // playback: A reads empty
		return;
	}
	if (offset < 0x800) { // FIFO B: Sonora only
		if (m_Sonora) {
			m_B.Push(value);
			if (m_B.count >= 0x200) {
				m_Status &= (u8)~0x04;
				if (m_B.count >= 0x3FF)
					m_Status |= 0x08; // full
			} else {
				m_Status &= (u8)~0x08;
			}
		}
		return;
	}
	switch (offset) {
	case 0x801: case 0x802: case 0x805: case 0x807:
		return; // read-only / nothing here
	case 0x803:
		if (value & 0x80) { // clear the FIFOs
			m_A.read = m_A.count = m_B.read = m_B.count = 0;
			if (!m_Sonora)
				UpdateStatus();
		}
		break;
	case 0x806:
		m_Volume = value;
		break;
	case 0xF09:
	case 0xF29:
		if (m_Sonora) {
			const unsigned i = offset == 0xF29;
			const u8 halfBit = i ? 0x04 : 0x01;
			if (!(value & 1) && (m_IrqDisable[i] & 1) && (m_Status & halfBit))
				m_Irq = true;
			else if (value & 1)
				m_Irq = false;
			m_IrqDisable[i] = value & 1;
		}
		return;
	default:
		break;
	}
	if (offset < 0x840)
		m_Regs[offset - 0x800] = value;
}

s16 AscV8::NextSample()
{
	if (m_Sonora) {
		u8 v;
		if (m_A.Pop(v))
			m_LastA = (s8)(v ^ 0x80);
		if (m_B.Pop(v))
			m_LastB = (s8)(v ^ 0x80);
		UpdateStatus();
		return (s16)(((int)m_LastA + (int)m_LastB) * 128);
	}
	u8 v;
	if (m_A.Pop(v)) {
		const s8 sample = (s8)(v ^ 0x80);
		const unsigned volume = (m_Volume >> 5) & 7;
		m_Last = (s16)(sample * 256 * (int)volume / 7); // as the Classic: full scale at 7
	}
	UpdateStatus(); // empty: stays "half empty", keeps asking
	return m_Last;
}

} // namespace pimac
