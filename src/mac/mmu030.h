// The 68030's paged MMU, emulated outside Musashi (whose own PMMU is
// incomplete): translation happens in the bus callbacks (mac/cpu.cpp), for
// every access including instruction fetches, through a small translation
// cache. Musashi decodes the MMU instructions (PMOVE, PFLUSH, PTEST, PLOAD)
// with mac/musashi_mmu.h, which calls the C hooks at the end of this file.
//
// Implemented: TC (page size, IS, TIA-TID; SRE), CRP/SRP, short and long
// descriptors, early termination, indirect descriptors, write protect, TT0/
// TT1 transparent windows, MMUSR via PTEST, used/modified bits (table
// searches set U; the first write to a page sets M, so the cache only
// holds writable entries for modified pages). A fault reports its address,
// SSW and output data to Musashi's 68030 bus fault frame (format $B); the
// CPU then restarts the instruction on RTE (virtual memory). Not
// implemented: function-code lookup (FCL), the CI (cache inhibit) bit.
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

// size: 1, 2 or 4 bytes (for the fault's SSW).
u32 TranslateSlow(u32 logical, bool write, unsigned size);

// Logical -> physical. On an invalid or write-protected page raises a bus
// error in the CPU (does not return).
inline u32 Translate(u32 logical, bool write, unsigned size)
{
	const u32 chunk = logical >> g_ChunkShift;
	const CacheEntry &e = g_Cache[chunk & ((1u << kCacheBits) - 1)];
	if ((write ? e.writeTag : e.readTag) == chunk + 1 && g_CacheUsable)
		return e.base | (logical & ((1u << g_ChunkShift) - 1));
	return TranslateSlow(logical, write, size);
}

// True if reads of `logical` translate through a cache entry (what
// Translate just used), so the whole chunk (1 << g_ChunkShift bytes) maps to
// one physical chunk until the next flush or MMU register write.
inline bool ReadCached(u32 logical)
{
	const u32 chunk = logical >> g_ChunkShift;
	return g_CacheUsable && g_Cache[chunk & ((1u << kCacheBits) - 1)].readTag == chunk + 1;
}

// For diagnostics: walks the tables (supervisor) without side effects.
bool Probe(u32 logical, u32 &physical, bool &writeProtected);
u32 Tc();
// True once after the MMU was switched off (reset, or the ROM restarting).
bool TakeDisabled();

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
// The last fault, for the bus fault frame (read by Musashi).
extern unsigned pimac_fault_address;
extern unsigned pimac_fault_ssw;
extern unsigned pimac_fault_data; // set by the write callbacks before translating
}
