#include "mac/mmu030.h"

#include "core/log.h"
#include "mac/cpu.h"

extern "C" {
#include "m68k.h"
}

namespace pimac {
namespace mmu {

bool g_Enabled = false;
CacheEntry g_Cache[1u << kCacheBits];
unsigned g_ChunkShift = 12;
bool g_CacheUsable = true;

namespace {

CpuBus *g_Bus = 0;

u32 g_Tc = 0;
u64 g_Crp = 0, g_Srp = 0;
u32 g_Tt[2] = {0, 0};
u16 g_MmuSr = 0;
u32 g_CacheBlock = 0; // instruction-cache stand-in (see pimac_mmu_write_reg)
bool g_CacheBlockValid = false;
bool g_NeedFc = false; // translation depends on the function code (SRE / FC-specific TT)
bool g_WasDisabled = true;


u32 Read32(u32 a)
{
	return (g_Bus->BusRead16(a) << 16) | g_Bus->BusRead16(a + 2);
}

// Sets U (bit 3), and M (bit 4) if asked, in the descriptor at `at` whose
// first long word is d0 (both bits are in its low word). Returns the new d0.
u32 MarkDescriptor(u32 at, u32 d0, bool modified)
{
	const u32 want = d0 | 8 | (modified ? 0x10 : 0);
	if (want != d0)
		g_Bus->BusWrite16(at + 2, want & 0xFFFF);
	return want;
}

void Flush()
{
	memset(g_Cache, 0, sizeof g_Cache);
	cpu::InvalidateFetch();
}

void Configure(bool flush)
{
	const unsigned oldShift = g_ChunkShift;
	g_Enabled = (g_Tc & 0x80000000u) != 0;
	if (!g_Enabled)
		g_WasDisabled = true;
	unsigned ps = (g_Tc >> 20) & 0xF;
	if (ps < 8)
		ps = 8;
	g_ChunkShift = ps < 12 ? ps : 12;
	g_NeedFc = (g_Tc & 0x02000000u) != 0;
	for (unsigned i = 0; i < 2; ++i)
		if ((g_Tt[i] & 0x8000) && (g_Tt[i] & 7) != 7)
			g_NeedFc = true; // FC-specific window
	g_CacheUsable = !g_NeedFc;
	if (flush || g_ChunkShift != oldShift)
		Flush();
	cpu::InvalidateFetch(); // enable, page size or function-code use may have changed
}

// Walks the tables for `logical` and enters the result in the cache.
bool Load(u32 logical, bool supervisor);

bool TransparentMatch(u32 tt, u32 logical, bool write, bool supervisor)
{
	if (!(tt & 0x8000))
		return false;
	const u32 base = tt >> 24, mask = (tt >> 16) & 0xFF;
	if (((logical >> 24) ^ base) & ~mask & 0xFF)
		return false;
	if (!(tt & 0x100) && (((tt >> 9) & 1) != (write ? 0u : 1u)))
		return false; // R/W must match unless RWM
	// FC: assume data/program of the current mode both match a window
	// whose FC base/mask covers the mode (S = FC bit 2).
	const u32 fcBase = (tt >> 4) & 7, fcMask = tt & 7;
	const u32 fc = supervisor ? 5 : 1;
	if (((fc ^ fcBase) & ~fcMask & 4) != 0)
		return false;
	return true;
}

// Result of a walk.
struct Walk {
	bool ok;
	bool wp;
	bool modified; // the page descriptor's M bit (after this walk)
	u32 physical;
	u32 lastDescriptor;
	unsigned levels;
};

// update: a real table search, which sets U in each descriptor it reads,
// and M in the page descriptor when `write` (and the page is writable).
// PTEST and diagnostics walk without changing anything.
Walk DoWalk(u32 logical, bool supervisor, bool update = false, bool write = false)
{
	Walk w;
	w.ok = false;
	w.wp = false;
	w.modified = false;
	w.physical = 0;
	w.lastDescriptor = 0;
	w.levels = 0;

	const u64 root = (supervisor && (g_Tc & 0x02000000u)) ? g_Srp : g_Crp;
	unsigned dt = (unsigned)(root >> 32) & 3;
	u32 table = (u32)root & 0xFFFFFFF0u;
	u32 limitWord = (u32)(root >> 32); // L/U + limit of the next table...
	bool hasLimit = true;              // ...if it was a long descriptor (the root always is)

	const unsigned ps = (g_Tc >> 20) & 0xF;
	const unsigned is = (g_Tc >> 16) & 0xF;
	const unsigned widths[4] = {(g_Tc >> 12) & 0xF, (g_Tc >> 8) & 0xF, (g_Tc >> 4) & 0xF,
				    g_Tc & 0xF};
	unsigned used = is; // address bits consumed so far (from the top)

	if (dt == 1) { // root is a page descriptor: no translation
		w.ok = true;
		w.modified = true;
		w.physical = logical;
		return w;
	}
	for (unsigned level = 0; level < 4; ++level) {
		const unsigned width = widths[level];
		if (dt == 0)
			return w; // invalid
		if (width == 0)
			break;
		if (used + width > 32)
			return w;
		const u32 index = used >= 32 ? 0 : (u32)(logical << used) >> (32 - width);
		used += width;
		if (hasLimit) { // the pointer to this table carried a limit
			const u32 limit = (limitWord >> 16) & 0x7FFF;
			if ((limitWord & 0x80000000u) ? index < limit : index > limit)
				return w; // limit violation
		}
		const unsigned size = dt == 3 ? 8 : 4;
		const u32 at = table + index * size;
		u32 d0 = Read32(at), d1 = size == 8 ? Read32(at + 4) : 0;
		w.lastDescriptor = at;
		++w.levels;
		unsigned ndt = d0 & 3;
		if (d0 & 4)
			w.wp = true;
		const u32 addr = size == 8 ? d1 : d0;
		if (update && ndt != 0)
			d0 = MarkDescriptor(at, d0, ndt == 1 && write && !w.wp);
		if (ndt == 1) { // page descriptor
			w.modified = (d0 & 0x10) != 0;
			const u32 offsetBits = 32 - used;
			const u32 pageMask = offsetBits >= 32 ? 0xFFFFFFFFu : ((1u << offsetBits) - 1);
			const bool last = level == 3 || widths[level + 1] == 0;
			// Early termination (above the last level): the remaining
			// logical bits are added to the page address, which need not
			// be aligned (the ROM maps its screen window that way).
			w.physical = last ? (addr & ~pageMask) + (logical & pageMask)
					  : (addr & 0xFFFFFF00u) + (logical & pageMask);
			w.ok = true;
			return w;
		}
		if (ndt == 0)
			return w;
		dt = ndt;
		table = addr & 0xFFFFFFF0u;
		limitWord = d0;
		hasLimit = size == 8;
		// Reached the last level with a table pointer: an indirect
		// descriptor pointing at the page descriptor.
		const bool last = level == 3 || widths[level + 1] == 0;
		if (last) {
			u32 pd = Read32(table);
			const u32 pa = (dt == 3) ? Read32(table + 4) : pd;
			w.lastDescriptor = table;
			if ((pd & 3) != 1)
				return w;
			if (pd & 4)
				w.wp = true;
			if (update)
				pd = MarkDescriptor(table, pd, write && !w.wp);
			w.modified = (pd & 0x10) != 0;
			const u32 pageMask = (1u << ps) - 1;
			w.physical = (pa & ~pageMask & 0xFFFFFF00u) + (logical & pageMask);
			w.ok = true;
			return w;
		}
	}
	return w;
}

} // namespace

bool Probe(u32 logical, u32 &physical, bool &writeProtected)
{
	if (TransparentMatch(g_Tt[0], logical, false, true) ||
	    TransparentMatch(g_Tt[1], logical, false, true)) {
		physical = logical;
		writeProtected = false;
		return true;
	}
	const Walk w = DoWalk(logical, true);
	physical = w.physical;
	writeProtected = w.wp;
	return w.ok;
}

u32 Tc()
{
	return g_Tc;
}

bool TakeDisabled()
{
	const bool d = g_WasDisabled;
	g_WasDisabled = false;
	return d;
}

void Attach(CpuBus *bus)
{
	g_Bus = bus;
}

void Reset()
{
	g_Tc = 0;
	g_Crp = g_Srp = 0;
	g_Tt[0] = g_Tt[1] = 0;
	g_MmuSr = 0;
	Configure(true);
}

// Writes hit the cache only on modified pages: the first write to a clean
// page takes the slow path, which sets M.
static void Fill(CacheEntry &e, u32 chunk, u32 physical, bool writable)
{
	e.readTag = chunk + 1;
	e.writeTag = writable ? chunk + 1 : 0;
	e.base = physical & ~((1u << g_ChunkShift) - 1);
}

u32 TranslateSlow(u32 logical, bool write, unsigned size)
{
	const bool supervisor = g_NeedFc ? pimac_cpu_supervisor() != 0 : true;
	if (TransparentMatch(g_Tt[0], logical, write, supervisor) ||
	    TransparentMatch(g_Tt[1], logical, write, supervisor))
		return logical;

	const Walk w = DoWalk(logical, supervisor, true, write);
	if (w.ok && !g_NeedFc) {
		const u32 chunk = logical >> g_ChunkShift;
		Fill(g_Cache[chunk & ((1u << kCacheBits) - 1)], chunk, w.physical,
		     !w.wp && w.modified);
	}
	if (!w.ok || (write && w.wp)) {
		if (g_CacheBlockValid) {
			if ((cpu::PC() & ~0xFFu) == g_CacheBlock && (logical & ~0xFFu) == g_CacheBlock)
				return logical; // still running from the instruction cache
			g_CacheBlockValid = false;
		}
		// For the 68030 bus fault frame: a data fault (DF, rerun on RTE),
		// R/W, size (long = 0) and function code (data space).
		pimac_fault_address = logical;
		pimac_fault_ssw = 0x100 | (write ? 0 : 0x40) | (size == 1 ? 0x10 : size == 2 ? 0x20 : 0) |
				  (pimac_cpu_supervisor() ? 5 : 1);
		m68k_pulse_bus_error(); // does not return
		return logical;
	}
	return w.physical;
}

namespace {
bool Load(u32 logical, bool supervisor)
{
	const Walk w = DoWalk(logical, supervisor, true, false);
	if (!w.ok)
		return false;
	const u32 chunk = logical >> g_ChunkShift;
	Fill(g_Cache[chunk & ((1u << kCacheBits) - 1)], chunk, w.physical, !w.wp && w.modified);
	return true;
}
} // namespace

} // namespace mmu
} // namespace pimac

using namespace pimac;

unsigned pimac_fault_address = 0;
unsigned pimac_fault_ssw = 0;
unsigned pimac_fault_data = 0;

extern "C" void pimac_mmu_write_reg(int reg, unsigned long long value, int flushDisable)
{
	switch (reg) {
	case PIMAC_MMU_TC: mmu::g_Tc = (u32)value; break;
	case PIMAC_MMU_SRP: mmu::g_Srp = value; break;
	case PIMAC_MMU_CRP: mmu::g_Crp = value; break;
	case PIMAC_MMU_TT0: mmu::g_Tt[0] = (u32)value; break;
	case PIMAC_MMU_TT1: mmu::g_Tt[1] = (u32)value; break;
	case PIMAC_MMU_MMUSR: mmu::g_MmuSr = (u16)value; return;
	default: return;
	}
	// The instructions after a PMOVE that changes the mapping are
	// typically already in the 68030's instruction cache (the ROM runs its
	// MMU switches twice to make sure), so they still execute from the old
	// addresses even if the new tables do not map them.
	mmu::g_CacheBlock = cpu::PC() & ~0xFFu;
	mmu::g_CacheBlockValid = true;
	mmu::Configure(!flushDisable);
}

extern "C" unsigned long long pimac_mmu_read_reg(int reg)
{
	switch (reg) {
	case PIMAC_MMU_TC: return mmu::g_Tc;
	case PIMAC_MMU_SRP: return mmu::g_Srp;
	case PIMAC_MMU_CRP: return mmu::g_Crp;
	case PIMAC_MMU_TT0: return mmu::g_Tt[0];
	case PIMAC_MMU_TT1: return mmu::g_Tt[1];
	case PIMAC_MMU_MMUSR: return mmu::g_MmuSr;
	default: return 0;
	}
}

extern "C" void pimac_mmu_flush(void)
{
	mmu::Flush();
}

extern "C" void pimac_mmu_load(unsigned logical, int supervisor)
{
	if (mmu::g_Enabled)
		mmu::Load(logical, supervisor != 0);
}

extern "C" unsigned pimac_mmu_test(unsigned logical, int write, int supervisor)
{
	u16 sr = 0;
	if (mmu::TransparentMatch(mmu::g_Tt[0], logical, write != 0, supervisor != 0) ||
	    mmu::TransparentMatch(mmu::g_Tt[1], logical, write != 0, supervisor != 0)) {
		mmu::g_MmuSr = 0x0040; // T
		return 0;
	}
	const mmu::Walk w = mmu::DoWalk(logical, supervisor != 0);
	if (!w.ok)
		sr |= 0x0400; // I
	if (w.wp)
		sr |= 0x0800; // W
	sr |= (u16)(w.levels & 7);
	mmu::g_MmuSr = sr;
	return w.lastDescriptor;
}
