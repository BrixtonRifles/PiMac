// IWM floppy controller. No floppy drives are emulated yet: every drive
// sense line reads 1, which the ROM interprets as "no drive / no disk".
#pragma once

#include "core/compat.h"

namespace pimac {

class Iwm {
public:
	Iwm() { Reset(); }

	void Reset()
	{
		m_Lines = 0;
		m_Mode = 0;
	}

	// Every access to IWM space toggles one control line; reads then return
	// the register selected by Q6/Q7.
	u8 Access(unsigned offset, bool write, u8 value)
	{
		const unsigned line = (offset >> 1) & 7;
		if (offset & 1)
			m_Lines |= (u8)(1u << line);
		else
			m_Lines &= (u8)~(1u << line);

		const bool q6 = (m_Lines & kQ6) != 0, q7 = (m_Lines & kQ7) != 0;
		const bool enable = (m_Lines & kEnable) != 0;

		if (write) {
			if (q6 && q7 && !enable)
				m_Mode = value & 0x1F;
			return 0xFF;
		}
		if (!q7 && q6) // status register
			return (u8)(0x80 /* sense: no drive */ | (enable ? 0x20 : 0) | (m_Mode & 0x1F));
		if (q7 && !q6) // handshake register: ready, no underrun
			return 0xFF;
		return 0xFF; // data register
	}

private:
	enum : u8 { kEnable = 1 << 4, kQ6 = 1 << 6, kQ7 = 1 << 7 };

	u8 m_Lines;
	u8 m_Mode;
};

} // namespace pimac
