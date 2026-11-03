// The sound part of the V8 / Eagle ASIC (Classic II): an Apple Sound Chip
// cut down to one 1 KB FIFO (channel A), played at one 8-bit sample per
// video line (22254.5 Hz on the Classic II). The Sound Manager keeps it fed
// from the "half empty" interrupt (pseudo-VIA CB1).
//
// Registers (offset from the chip base): 0x000-0x3FF FIFO A, 0x800 version
// (0xE8), 0x804 FIFO status (bit 0 half empty, bit 1 empty or full), 0x806 volume
// (bits 5-7). Behaviour from MAME's asc.cpp (asc_v8_device) and Doug Brown's
// hardware tests quoted there.
#pragma once

#include "core/compat.h"

namespace pimac {

class AscV8 {
public:
	AscV8();
	void Reset();

	u8 Read(u32 offset);
	void Write(u32 offset, u8 value);

	// One output sample (a video line): signed, scaled by the volume.
	s16 NextSample();
	bool Irq() const { return m_Irq; } // "half empty", level

private:
	void UpdateStatus();

	u8 m_Fifo[1024];
	unsigned m_Read, m_Count;
	u8 m_Status; // 0x804
	u8 m_Volume; // 0x806
	u8 m_Regs[0x40];
	bool m_Irq;
	s16 m_Last;
};

} // namespace pimac
