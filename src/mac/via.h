// Rockwell/Synertek 6522 VIA, as used in the compact Macs.
// Timers run at the E clock (CPU clock / 10) and are evaluated lazily from
// the emulated CPU cycle count.
#pragma once

#include "core/compat.h"

namespace pimac {

class ViaListener {
public:
	virtual u8 ViaInputA() = 0; // pin levels for bits configured as inputs
	virtual u8 ViaInputB() = 0;
	virtual void ViaOutputA(u8 ora, u8 ddra) = 0;
	virtual void ViaOutputB(u8 orb, u8 ddrb) = 0;
	virtual void ViaIrq(bool asserted) = 0;
	virtual void ViaTimingChanged() = 0; // a timer was (re)loaded

protected:
	~ViaListener() {}
};

class Via6522 {
public:
	enum : u8 {
		IfrCA2 = 0x01,
		IfrCA1 = 0x02,
		IfrSR = 0x04,
		IfrCB2 = 0x08,
		IfrCB1 = 0x10,
		IfrT2 = 0x20,
		IfrT1 = 0x40,
	};
	static const u32 kCyclesPerTick = 10;
	static const u64 kNever = ~(u64)0;

	explicit Via6522(ViaListener *listener);

	void Reset();
	u8 Read(unsigned reg, u64 now);
	void Write(unsigned reg, u8 value, u64 now);

	// Raise timer interrupts that are due at or before `now` (CPU cycles).
	void Update(u64 now);
	u64 NextEvent() const; // CPU cycle of the next timer interrupt

	void SignalCA1() { SetFlags(IfrCA1); }
	void SignalCA2() { SetFlags(IfrCA2); }

	// Shift register traffic from an external device (ADB transceiver).
	void DeviceShiftIn(u8 value); // device -> SR, raises the SR interrupt
	u8 DeviceShiftOut();          // SR -> device, raises the SR interrupt
	u8 ShiftRegister() const { return m_Sr; }

	u8 Ifr() const { return m_Ifr; }
	u8 Ier() const { return m_Ier; }

private:
	void SetFlags(u8 flags);
	void ClearFlags(u8 flags);
	void UpdateIrq();
	bool CA2Independent() const { return (m_Pcr & 0x0A) == 0x02; }
	bool CB2Independent() const { return (m_Pcr & 0xA0) == 0x20; }
	u8 PortAValue() const;
	u8 PortBValue() const;

	ViaListener *m_pListener;
	u8 m_Ora, m_Orb, m_Ddra, m_Ddrb;
	u8 m_Acr, m_Pcr, m_Ifr, m_Ier, m_Sr;
	bool m_Irq;

	// Timer 1
	u16 m_T1Latch;
	u16 m_T1Start;  // counter value at m_T1Base
	u64 m_T1Base;   // E tick
	u64 m_T1Next;   // E tick of next underflow interrupt
	bool m_T1Armed;
	// Timer 2
	u8 m_T2LatchLow;
	u16 m_T2Start;
	u64 m_T2Base;
	u64 m_T2Next;
	bool m_T2Armed;
};

} // namespace pimac
