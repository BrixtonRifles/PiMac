// Zilog 8530 SCC (serial ports). Minimal: register file only, transmitter
// always empty, no receive data and no interrupts. Enough for the ROM to
// initialise the serial ports and carry on.
#pragma once

#include "core/compat.h"

namespace pimac {

class Scc {
public:
	Scc() { Reset(); }

	void Reset()
	{
		memset(m_Wr, 0, sizeof m_Wr);
		m_Pointer[0] = m_Pointer[1] = 0;
	}

	// channel: 0 = B, 1 = A; data: true for the data register
	u8 Read(unsigned channel, bool data)
	{
		channel &= 1;
		if (data)
			return 0;
		unsigned reg = m_Pointer[channel];
		m_Pointer[channel] = 0;
		switch (reg) {
		case 0:
			return 0x44; // Tx underrun/EOM, Tx buffer empty
		case 1:
			return 0x01; // all sent
		case 2:
			return m_Wr[0][2]; // interrupt vector
		case 3:
			return 0; // no interrupts pending
		case 10:
			return 0;
		case 12:
			return m_Wr[channel][12];
		case 13:
			return m_Wr[channel][13];
		case 15:
			return m_Wr[channel][15];
		default:
			return 0;
		}
	}

	void Write(unsigned channel, bool data, u8 value)
	{
		channel &= 1;
		if (data)
			return; // transmitted bytes go nowhere
		unsigned reg = m_Pointer[channel];
		if (reg == 0) {
			m_Pointer[channel] = (value & 7) | (((value >> 3) & 7) == 1 ? 8 : 0);
			return;
		}
		m_Pointer[channel] = 0;
		if (reg == 2 || reg == 9) { // shared registers
			m_Wr[0][reg] = m_Wr[1][reg] = value;
			if (reg == 9 && (value & 0xC0) == 0xC0)
				Reset(); // hardware reset
			return;
		}
		m_Wr[channel][reg] = value;
	}

private:
	u8 m_Wr[2][16];
	unsigned m_Pointer[2];
};

} // namespace pimac
