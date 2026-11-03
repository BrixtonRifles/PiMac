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
