// The sound part of the V8-family ASICs: an Apple Sound Chip cut down to
// FIFO playback at 22254.5 Hz (one sample every 704 CPU cycles at
// 15.6672 MHz). The Sound Manager keeps it fed from the "half empty"
// interrupt (pseudo-VIA CB1).
//
//   V8 / Eagle (Classic II): one 1 KB FIFO (A, mono), version 0xE8,
//     0x804 status bit 0 = A half empty, bit 1 = A empty or full; 0x806
//     volume (bits 5-7).
//   Sonora flavour (Spice, Color Classic): FIFOs A (left) and B (right),
//     version 0xBC; status bit 2 = either FIFO half empty (the interrupt,
//     unless 0xF29 bit 0 disables it), bit 3 = either empty, and set by a
//     write that fills B (the boot chime waits for it); bit 1 always set
//     while playing; 0xF09 / 0xF29 = FIFO A / B interrupt disable (bit 0;
//     enabled at reset, and A then interrupts all the time). 0x806 holds a
//     value but does not scale the output (the OS only probes it).
//
// Behaviour from MAME's asc.cpp (asc_v8_device, asc_sonora_device) and Doug
// Brown's hardware tests quoted there.
#pragma once

#include "core/compat.h"

namespace pimac {

class AscV8 {
public:
	explicit AscV8(bool sonora = false);
	void Reset();
	void SetSonora(bool sonora) { m_Sonora = sonora; Reset(); }

	u8 Read(u32 offset);
	void Write(u32 offset, u8 value);

	// One output sample: mono (Sonora: the average of A and B), signed.
	s16 NextSample();
	bool Irq() const { return m_Irq; } // level

private:
	struct Fifo {
		u8 data[1024];
		unsigned read, count;
		bool Push(u8 v);
		bool Pop(u8 &v);
	};
	void UpdateStatus();

	bool m_Sonora;
	Fifo m_A, m_B;
	u8 m_Status; // 0x804
	u8 m_Volume; // 0x806
	u8 m_IrqDisable[2]; // Sonora 0xF09 / 0xF29 (bit 0)
	u8 m_Regs[0x40];
	bool m_Irq;
	s16 m_Last;
	s8 m_LastA, m_LastB;
};

} // namespace pimac

namespace pimac {

// The original Apple Sound Chip (344S0063) of the Mac II family and SE/30,
// after MAME's asc_device: version 0x00, two 1 KB FIFOs (A left, B right)
// played at 22257 Hz in FIFO mode (0x801 = 1), or four wavetable voices
// (0x801 = 2: 512-byte tables, A low/high then B low/high, 9.15 fixed-point
// phase/increment at 0x810-0x82F). 0x804 status (bit 0 A half empty, 1 A
// empty/full, 2/3 the same for B) clears on read, and so does the interrupt,
// which is an edge (VIA2 CB1) raised when a FIFO drops to half empty.
class AscClassic {
public:
	AscClassic() { Reset(); }
	void Reset();

	u8 Read(u32 offset);
	void Write(u32 offset, u8 value);

	s16 NextSample(); // mono mix
	bool Irq() const { return m_Irq; }

private:
	u8 m_Fifo[2][1024];
	unsigned m_Rd[2], m_Wr[2], m_Cap[2];
	u8 m_Regs[0x800]; // 0x800-0xFFF
	u32 m_Phase[4], m_Incr[4];
	bool m_Irq;
	s16 m_Last;
};

} // namespace pimac
