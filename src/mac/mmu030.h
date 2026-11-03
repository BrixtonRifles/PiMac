// The 68030's paged MMU, emulated outside Musashi (whose own PMMU is
// incomplete): translation happens in the bus callbacks (mac/cpu.cpp), for
// every access including instruction fetches, through a small translation
// cache. Musashi decodes the MMU instructions (PMOVE, PFLUSH, PTEST, PLOAD)
// with mac/musashi_mmu.h, which calls the C hooks at the end of this file.
//
// Implemented: TC (page size, IS, TIA-TID; SRE), CRP/SRP, short and long
// descriptors, early termination, indirect descriptors, write protect, TT0/
// TT1 transparent windows, MMUSR via PTEST. Not implemented: limits,
// function-code lookup (FCL), used/modified bits, the CI (cache inhibit) bit.
#pragma once

#include "core/compat.h"

namespace pimac {

class CpuBus;

namespace mmu {

void Attach(CpuBus *bus); // where page tables are read from (physical)
void Reset();             // MMU off, registers cleared
extern bool g_Enabled;
inline bool Enabled() { return g_Enabled; }

// Translation cache (inline fast path below): direct-mapped, one entry per
// min(page, 4 KB) chunk. Tags are chunk number + 1 (0 = empty); writeTag is
// 0 for write-protected pages, so writes there take the slow path.
struct CacheEntry {
	u32 readTag;
	u32 writeTag;
	u32 base; // physical address of the chunk
};
const unsigned kCacheBits = 10;
extern CacheEntry g_Cache[1u << kCacheBits];
extern unsigned g_ChunkShift;
extern bool g_CacheUsable; // false while translation depends on the function code

u32 TranslateSlow(u32 logical, bool write);

// Logical -> physical. On an invalid or write-protected page raises a bus
// error in the CPU (does not return).
inline u32 Translate(u32 logical, bool write)
{
	const u32 chunk = logical >> g_ChunkShift;
	const CacheEntry &e = g_Cache[chunk & ((1u << kCacheBits) - 1)];
	if ((write ? e.writeTag : e.readTag) == chunk + 1 && g_CacheUsable)
		return e.base | (logical & ((1u << g_ChunkShift) - 1));
	return TranslateSlow(logical, write);
}

// For diagnostics: walks the tables (supervisor) without side effects.
bool Probe(u32 logical, u32 &physical, bool &writeProtected);
u32 Tc();

} // namespace mmu
} // namespace pimac

extern "C" {
// Called from Musashi's MMU instruction decoder.
enum {
	PIMAC_MMU_TC = 0, PIMAC_MMU_SRP = 1, PIMAC_MMU_CRP = 2,
	PIMAC_MMU_TT0 = 3, PIMAC_MMU_TT1 = 4, PIMAC_MMU_MMUSR = 5
};
void pimac_mmu_write_reg(int reg, unsigned long long value, int flushDisable);
unsigned long long pimac_mmu_read_reg(int reg);
void pimac_mmu_flush(void);
void pimac_mmu_load(unsigned logical, int supervisor); // PLOAD
// PTEST: walks the tables for `logical`, sets MMUSR; returns the address of
// the last descriptor read (for PTEST's optional An).
unsigned pimac_mmu_test(unsigned logical, int write, int supervisor);
int pimac_cpu_supervisor(void); // provided by musashi_mmu.h
}
