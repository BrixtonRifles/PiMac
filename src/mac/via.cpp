#include "mac/via.h"

namespace pimac {

Via6522::Via6522(ViaListener *listener) : m_pListener(listener)
{
	Reset();
}

void Via6522::Reset()
{
	m_Ora = m_Orb = m_Ddra = m_Ddrb = 0;
	m_Acr = m_Pcr = m_Ifr = m_Ier = m_Sr = 0;
	m_Irq = false;
	m_T1Latch = 0xFFFF;
	m_T1Start = 0xFFFF;
	m_T1Base = m_T1Next = 0;
	m_T1Armed = false;
	m_T2LatchLow = 0xFF;
	m_T2Start = 0xFFFF;
	m_T2Base = m_T2Next = 0;
	m_T2Armed = false;
	UpdateIrq();
}

void Via6522::SetFlags(u8 flags)
{
	m_Ifr |= flags & 0x7F;
	UpdateIrq();
}

void Via6522::ClearFlags(u8 flags)
{
	m_Ifr &= (u8)~flags;
	UpdateIrq();
}

void Via6522::UpdateIrq()
{
	bool irq = (m_Ifr & m_Ier & 0x7F) != 0;
	if (irq != m_Irq) {
		m_Irq = irq;
		m_pListener->ViaIrq(irq);
	}
}

u8 Via6522::PortAValue() const
{
	return (u8)((m_Ora & m_Ddra) | (m_pListener->ViaInputA() & ~m_Ddra));
}

u8 Via6522::PortBValue() const
{
	return (u8)((m_Orb & m_Ddrb) | (m_pListener->ViaInputB() & ~m_Ddrb));
}

void Via6522::Update(u64 now)
{
	const u64 tick = now / kCyclesPerTick;

	if (m_T1Armed && tick >= m_T1Next) {
		SetFlags(IfrT1);
		if (m_Acr & 0x40) {
			// Free-running: reload from the latch every latch + 2 ticks.
			const u64 period = (u64)m_T1Latch + 2;
			u64 periods = (tick - m_T1Next) / period + 1;
			m_T1Base = m_T1Next + 1 + (periods - 1) * period;
			m_T1Start = m_T1Latch;
			m_T1Next = m_T1Base + m_T1Latch + 1;
		} else {
			m_T1Armed = false;
		}
	}

	if (m_T2Armed && tick >= m_T2Next) {
		SetFlags(IfrT2);
		m_T2Armed = false;
	}
}

u64 Via6522::NextEvent() const
{
	u64 next = kNever;
	if (m_T1Armed)
		next = m_T1Next * kCyclesPerTick;
	if (m_T2Armed && m_T2Next * kCyclesPerTick < next)
		next = m_T2Next * kCyclesPerTick;
	return next;
}

void Via6522::DeviceShiftIn(u8 value)
{
	m_Sr = value;
	SetFlags(IfrSR);
}

u8 Via6522::DeviceShiftOut()
{
	SetFlags(IfrSR);
	return m_Sr;
}

u8 Via6522::Read(unsigned reg, u64 now)
{
	Update(now);
	const u64 tick = now / kCyclesPerTick;

	switch (reg & 0xF) {
	case 0x0: // ORB / IRB
		ClearFlags((u8)(IfrCB1 | (CB2Independent() ? 0 : IfrCB2)));
		return PortBValue();
	case 0x1: // ORA / IRA with handshake
		ClearFlags((u8)(IfrCA1 | (CA2Independent() ? 0 : IfrCA2)));
		return PortAValue();
	case 0x2:
		return m_Ddrb;
	case 0x3:
		return m_Ddra;
	case 0x4: { // T1 counter low (clears T1 flag)
		u16 c = (u16)(m_T1Start - (u16)(tick - m_T1Base));
		ClearFlags(IfrT1);
		return (u8)c;
	}
	case 0x5:
		return (u8)((u16)(m_T1Start - (u16)(tick - m_T1Base)) >> 8);
	case 0x6:
		return (u8)m_T1Latch;
	case 0x7:
		return (u8)(m_T1Latch >> 8);
	case 0x8: { // T2 counter low (clears T2 flag)
		u16 c = (u16)(m_T2Start - (u16)(tick - m_T2Base));
		ClearFlags(IfrT2);
		return (u8)c;
	}
	case 0x9:
		return (u8)((u16)(m_T2Start - (u16)(tick - m_T2Base)) >> 8);
	case 0xA:
		ClearFlags(IfrSR);
		return m_Sr;
	case 0xB:
		return m_Acr;
	case 0xC:
		return m_Pcr;
	case 0xD:
		return (u8)(m_Ifr | (m_Irq ? 0x80 : 0));
	case 0xE:
		return (u8)(m_Ier | 0x80);
	case 0xF: // ORA without handshake
	default:
		return PortAValue();
	}
}

void Via6522::Write(unsigned reg, u8 value, u64 now)
{
	Update(now);
	const u64 tick = now / kCyclesPerTick;

	switch (reg & 0xF) {
	case 0x0:
		m_Orb = value;
		ClearFlags((u8)(IfrCB1 | (CB2Independent() ? 0 : IfrCB2)));
		m_pListener->ViaOutputB(m_Orb, m_Ddrb);
		break;
	case 0x1:
		m_Ora = value;
		ClearFlags((u8)(IfrCA1 | (CA2Independent() ? 0 : IfrCA2)));
		m_pListener->ViaOutputA(m_Ora, m_Ddra);
		break;
	case 0x2:
		m_Ddrb = value;
		m_pListener->ViaOutputB(m_Orb, m_Ddrb);
		break;
	case 0x3:
		m_Ddra = value;
		m_pListener->ViaOutputA(m_Ora, m_Ddra);
		break;
	case 0x4:
	case 0x6:
		m_T1Latch = (u16)((m_T1Latch & 0xFF00) | value);
		break;
	case 0x5: // T1 counter high: load counter from latch and start
		m_T1Latch = (u16)((m_T1Latch & 0x00FF) | (value << 8));
		m_T1Start = m_T1Latch;
		m_T1Base = tick;
		m_T1Next = tick + m_T1Latch + 1;
		m_T1Armed = true;
		ClearFlags(IfrT1);
		m_pListener->ViaTimingChanged();
		break;
	case 0x7:
		m_T1Latch = (u16)((m_T1Latch & 0x00FF) | (value << 8));
		ClearFlags(IfrT1);
		break;
	case 0x8:
		m_T2LatchLow = value;
		break;
	case 0x9: // T2 counter high: load and start (one-shot)
		m_T2Start = (u16)((value << 8) | m_T2LatchLow);
		m_T2Base = tick;
		m_T2Next = tick + m_T2Start + 1;
		m_T2Armed = true;
		ClearFlags(IfrT2);
		m_pListener->ViaTimingChanged();
		break;
	case 0xA:
		m_Sr = value;
		ClearFlags(IfrSR);
		break;
	case 0xB:
		m_Acr = value;
		break;
	case 0xC:
		m_Pcr = value;
		break;
	case 0xD:
		ClearFlags(value & 0x7F);
		break;
	case 0xE:
		if (value & 0x80)
			m_Ier |= value & 0x7F;
		else
			m_Ier &= (u8)~value;
		UpdateIrq();
		break;
	case 0xF:
	default:
		m_Ora = value;
		m_pListener->ViaOutputA(m_Ora, m_Ddra);
		break;
	}
}

} // namespace pimac
