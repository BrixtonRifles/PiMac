// Thin wrapper around the Musashi 68000 core (a single global CPU).
#pragma once

#include "core/compat.h"

namespace pimac {

class CpuBus {
public:
	virtual u32 BusRead8(u32 address) = 0;
	virtual u32 BusRead16(u32 address) = 0;
	virtual void BusWrite8(u32 address, u32 value) = 0;
	virtual void BusWrite16(u32 address, u32 value) = 0;

protected:
	~CpuBus() {}
};

namespace cpu {

void Init(CpuBus *bus);  // 68000, then Reset()
void SetType(unsigned musashiType); // M68K_CPU_TYPE_* (for benchmarks; then Reset())
void Reset();            // reads SSP/PC from address 0/4 (and resets the FPU)
// A 68882 FPU on the coprocessor bus (src/fpu; off after Init). Without one,
// FPU opcodes take the F-line exception and the FPU probe bus-errors.
void SetFpu(bool present);
// Snapshot registers before each instruction, so that a bus fault (a page
// fault under virtual memory) restarts it on RTE. Costs ~20% speed.
void SetRollback(bool on);
// Emulated speed in percent of the real CPU (100 after Init). Above 100 the
// CPU gets that much more of its own cycles per machine cycle, so programs
// run faster while the clock, sound and video keep real time.
void SetSpeedPercent(unsigned percent);
// Fast memory: 64 KB pages of the physical address space that are plain
// memory (RAM, ROM, video RAM: no side effects on access). The CPU reads and
// writes them directly instead of calling the CpuBus, which is most of the
// emulator's time otherwise. A page with no pointer goes to the bus (write
// 0 for read-only memory such as ROM, so writes still reach the bus and are
// ignored there). Init clears the map; a machine fills it after each change
// to its memory map and must clear pages before they stop being plain memory.
const unsigned kFastPageShift = 16;
const u32 kFastPageSize = 1u << kFastPageShift;
void ClearFastMap();
void MapFastPage(u32 address, const u8 *read, u8 *write); // address: page aligned
// Instruction fetches remember the page they last came from (logical
// address -> host memory). The fast map and the MMU call this whenever a
// translation may have changed.
void InvalidateFetch();

int Execute(int cycles); // machine cycles to run; returns machine cycles run
int CyclesRun();         // machine cycles run so far in the current Execute()
void EndTimeslice();     // make Execute() return after this instruction
void SetIrq(unsigned level);
u32 PC();
u32 SP();
u32 SR();
u32 D(unsigned n);
u32 A(unsigned n);

} // namespace cpu
} // namespace pimac
