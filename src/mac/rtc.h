// Real-time clock + parameter RAM chip (with 256-byte extended PRAM, as in
// the SE and Classic), driven bit-serially through VIA port B:
// PB0 data, PB1 clock, PB2 /enable.
#pragma once

#include "core/compat.h"

namespace pimac {

class Rtc {
public:
	static const unsigned kPramSize = 256;

	Rtc();

	void Reset(); // bus state only; time and PRAM survive
	void SetSeconds(u32 secondsSince1904) { m_Seconds = secondsSince1904; }
	u32 Seconds() const { return m_Seconds; }
	void Tick() { ++m_Seconds; }

	// Called whenever VIA port B changes. `pins` holds the levels the RTC
	// sees on PB0..PB2 (PB0 only meaningful when the Mac drives it).
	void PinsChanged(u8 pins, bool macDrivesData);
	// Level the RTC drives on PB0 (when the Mac has PB0 as an input).
	u8 DataBit() const { return m_OutBit; }

	void SetDefaultPram(); // AppleTalk off; used until a saved PRAM is loaded
	u8 *Pram() { return m_Pram; }
	bool PramDirty() const { return m_PramDirty; }
	bool TakePramDirty()
	{
		bool d = m_PramDirty;
		m_PramDirty = false;
		return d;
	}

private:
	enum Mode { ModeCommand, ModeCommand2, ModeWrite, ModeRead, ModeDone };

	void ByteReceived(u8 byte);
	u8 ReadRegister(u8 cmd) const;
	void WriteRegister(u8 cmd, u8 value);
	int PramAddress(u8 cmd) const; // -1 if not a PRAM command

	u32 m_Seconds;
	u8 m_Pram[kPramSize];
	bool m_PramDirty;
	bool m_WriteProtect;

	bool m_Enabled;
	bool m_Clock;
	Mode m_Mode;
	u8 m_Shift;
	unsigned m_Bits;
	u8 m_Cmd;
	int m_ExtAddress;
	u8 m_OutBit;
};

} // namespace pimac
