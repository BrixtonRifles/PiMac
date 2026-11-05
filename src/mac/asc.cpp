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

namespace pimac {

void AscClassic::Reset()
{
	memset(m_Fifo, 0x80, sizeof m_Fifo);
	m_Rd[0] = m_Rd[1] = m_Wr[0] = m_Wr[1] = m_Cap[0] = m_Cap[1] = 0;
	memset(m_Regs, 0, sizeof m_Regs);
	memset(m_Phase, 0, sizeof m_Phase);
	memset(m_Incr, 0, sizeof m_Incr);
	m_Irq = false;
	m_Last = 0;
}

u8 AscClassic::Read(u32 offset)
{
	offset &= 0xFFF;
	if (offset < 0x800)
		return m_Fifo[offset >> 10][offset & 0x3FF];
	switch (offset) {
	case 0x800:
		return 0x00; // version: original ASC
	case 0x804: {
		const u8 v = m_Regs[0x04];
		m_Regs[0x04] = 0; // reading clears every bit and the interrupt
		m_Irq = false;
		return v;
	}
	case 0xF09:
	case 0xF29:
		return 0x01;
	default:
		break;
	}
	if (offset >= 0x810 && offset < 0x830) {
		const unsigned voice = (offset - 0x810) >> 3;
		const u32 v = (offset & 4) ? m_Incr[voice] : m_Phase[voice];
		return (u8)(v >> (8 * (3 - (offset & 3))));
	}
	return m_Regs[offset - 0x800];
}

void AscClassic::Write(u32 offset, u8 value)
{
	offset &= 0xFFF;
	if (offset < 0x800) {
		const unsigned f = offset >> 10;
		if ((m_Regs[0x01] & 3) != 1) {
			// Not in FIFO mode: the sample RAM is plain memory. The boot
			// chime (ROM 97221136 at 0x5E70) builds its four wave tables
			// with addressed, interleaved writes and averages them in place
			// to make the decay.
			m_Fifo[f][offset & 0x3FF] = value;
			return;
		}
		m_Fifo[f][m_Wr[f]] = value;
		m_Wr[f] = (m_Wr[f] + 1) & 0x3FF;
		if (m_Cap[f] < 0x400)
			++m_Cap[f];
		const u8 half = f ? 0x04 : 0x01, emptyFull = f ? 0x08 : 0x02;
		if (m_Cap[f] >= 0x200) {
			m_Regs[0x04] &= (u8)~half;
			if (m_Cap[f] >= 0x3FF)
				m_Regs[0x04] |= emptyFull;
		} else if (m_Cap[f]) {
			m_Regs[0x04] &= (u8)~emptyFull;
		}
		return;
	}
	if (offset == 0xE00) {
		m_Regs[0x04] |= 0x0F;
		m_Irq = true;
	}
	switch (offset) {
	case 0x801:
		value &= 3;
		if (value != m_Regs[0x01])
			m_Rd[0] = m_Rd[1] = m_Wr[0] = m_Wr[1] = m_Cap[0] = m_Cap[1] = 0;
		break;
	case 0x803:
		if (value & 0x80) {
			m_Rd[0] = m_Rd[1] = m_Wr[0] = m_Wr[1] = m_Cap[0] = m_Cap[1] = 0;
			m_Regs[0x04] |= 0x0A; // both empty
		}
		break;
	default:
		if (offset >= 0x810 && offset < 0x830 && (offset & 3)) {
			const unsigned voice = (offset - 0x810) >> 3;
			u32 &v = (offset & 4) ? m_Incr[voice] : m_Phase[voice];
			const unsigned shift = 8 * (3 - (offset & 3));
			v = (v & ~(0xFFu << shift)) | ((u32)value << shift);
		}
		break;
	}
	m_Regs[offset - 0x800] = value;
}

s16 AscClassic::NextSample()
{
	const unsigned volume = (m_Regs[0x06] >> 5) & 7;
	switch (m_Regs[0x01] & 3) {
	case 1: { // FIFO
		const bool stereo = (m_Regs[0x02] & 0x02) != 0;
		const int l = (s8)(m_Fifo[0][m_Rd[0]] ^ 0x80);
		const int r = stereo ? (s8)(m_Fifo[1][m_Rd[1]] ^ 0x80) : l;
		for (unsigned f = 0; f < (stereo ? 2u : 1u); ++f) {
			const unsigned cap = m_Cap[f];
			if (m_Cap[f]) {
				m_Rd[f] = (m_Rd[f] + 1) & 0x3FF;
				--m_Cap[f];
			}
			if (cap == 0x1FF) {
				m_Regs[0x04] |= f ? 0x04 : 0x01;    // half empty
				m_Regs[0x04] &= f ? 0xF7 : 0xFD;    // not empty/full
				m_Irq = true;
			}
		}
		m_Last = (s16)((l + r) * 128 * (int)volume / 7);
		break;
	}
	case 2: { // wavetable: four voices
		int mix = 0;
		for (unsigned v = 0; v < 4; ++v) {
			m_Phase[v] = (m_Phase[v] + m_Incr[v]) & 0xFFFFFF;
			const u8 s = m_Fifo[v >> 1][((m_Phase[v] >> 15) & 0x1FF) + ((v & 1) ? 0x200 : 0)];
			mix += (s8)(s ^ 0x80);
		}
		m_Last = (s16)(mix * 64 * (int)volume / 7);
		break;
	}
	default:
		break; // off: holds the last level
	}
	return m_Last;
}

} // namespace pimac
