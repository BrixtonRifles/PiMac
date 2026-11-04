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
void Reset();            // reads SSP/PC from address 0/4
// Snapshot registers before each instruction, so that a bus fault (a page
// fault under virtual memory) restarts it on RTE. Costs ~20% speed.
void SetRollback(bool on);
int Execute(int cycles); // returns cycles actually run
int CyclesRun();         // cycles run so far in the current Execute()
void EndTimeslice();     // make Execute() return after this instruction
void SetIrq(unsigned level);
u32 PC();
u32 SP();
u32 SR();
u32 D(unsigned n);
u32 A(unsigned n);

} // namespace cpu
} // namespace pimac
