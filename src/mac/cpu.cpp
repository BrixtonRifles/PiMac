#include "mac/cpu.h"

#include "mac/mmu030.h"

extern "C" {
#include "m68k.h"
// src/fpu/fpu68882.cpp (read by Musashi's FPU opcodes, scripts/musashi.patch)
extern int pimac_fpu_present;
void pimac_fpu_reset(void);
}

namespace {
pimac::CpuBus *g_Bus = 0;

// The fast map (see cpu.h): one entry per 64 KB page, allocated in Init.
const unsigned kFastPages = 1u << (32 - pimac::cpu::kFastPageShift);
const unsigned kFastOffsetMask = pimac::cpu::kFastPageSize - 1;
const u8 **g_FastRead = 0;
u8 **g_FastWrite = 0;

inline unsigned int Phys(unsigned int address, bool write, unsigned size)
{
	return pimac::mmu::Enabled() ? pimac::mmu::Translate(address, write, size) : address;
}

// Fetch cache: the page instruction fetches last came from. g_FetchPage is
// its logical address (1 = none: never page aligned), g_FetchHost the host
// memory behind it, g_FetchMask its size - 1: the fast map's 64 KB page with
// the MMU off, else the MMU's translation chunk (at most 4 KB). Only filled
// from fast-map pages (plain memory, no side effects) whose translation is
// in the MMU's cache, and dropped whenever either may change, so a fetch
// reads exactly what m68k_read_memory_16 would.
unsigned int g_FetchPage = 1;
unsigned int g_FetchMask = 0;
const u8 *g_FetchHost = 0;
unsigned int g_AddressMask = 0x00FFFFFF; // 24-bit 68000; PC-relative reads are not masked by Musashi

// The slow path, as m68k_read_memory_16, also remembering the page if it is
// plain memory.
unsigned int FetchSlow16(unsigned int address)
{
	const unsigned int a = Phys(address, false, 2);
	const unsigned int off = a & kFastOffsetMask;
	const u8 *p = g_FastRead[a >> pimac::cpu::kFastPageShift];
	if (!p || off == kFastOffsetMask)
		return g_Bus->BusRead16(a);
	unsigned int mask = kFastOffsetMask;
	if (!pimac::mmu::Enabled() || pimac::mmu::ReadCached(address)) {
		if (pimac::mmu::Enabled())
			mask = (1u << pimac::mmu::g_ChunkShift) - 1;
		g_FetchPage = address & ~mask;
		g_FetchMask = mask;
		g_FetchHost = p + (off & ~mask);
	}
	return ((unsigned int)p[off] << 8) | p[off + 1];
}

inline unsigned int Phys32(unsigned int &second, unsigned int address, bool write)
{
	// A long access may cross a page. (A fault on its second half reports
	// the long access: the instruction is restarted either way.)
	const unsigned int first = Phys(address, write, 4);
	second = ((address ^ (address + 2)) & ~0xFFu) ? Phys(address + 2, write, 4) : first + 2;
	return first;
}
} // namespace

// Musashi memory callbacks. 32-bit accesses are split into two 16-bit bus
// cycles, as on the real 68000.
extern "C" {

// The fast map is tried first; an access that runs off the end of its page
// (an odd word or long at the end) takes the bus, as before.

unsigned int m68k_read_memory_8(unsigned int address)
{
	const unsigned int a = Phys(address, false, 1);
	if (const u8 *p = g_FastRead[a >> pimac::cpu::kFastPageShift])
		return p[a & kFastOffsetMask];
	return g_Bus->BusRead8(a);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
	const unsigned int a = Phys(address, false, 2);
	const unsigned int off = a & kFastOffsetMask;
	const u8 *p = g_FastRead[a >> pimac::cpu::kFastPageShift];
	if (p && off < kFastOffsetMask) {
		p += off;
		return ((unsigned int)p[0] << 8) | p[1];
	}
	return g_Bus->BusRead16(a);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
	unsigned int second;
	const unsigned int first = Phys32(second, address, false);
	const unsigned int off = first & kFastOffsetMask;
	const u8 *p = g_FastRead[first >> pimac::cpu::kFastPageShift];
	if (p && second == first + 2 && off <= kFastOffsetMask - 3) {
		p += off;
		return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
	}
	unsigned int hi = g_Bus->BusRead16(first);
	return (hi << 16) | g_Bus->BusRead16(second);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
	pimac_fault_data = value;
	const unsigned int a = Phys(address, true, 1);
	if (u8 *p = g_FastWrite[a >> pimac::cpu::kFastPageShift]) {
		p[a & kFastOffsetMask] = (u8)value;
		return;
	}
	g_Bus->BusWrite8(a, value & 0xFF);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
	pimac_fault_data = value;
	const unsigned int a = Phys(address, true, 2);
	const unsigned int off = a & kFastOffsetMask;
	u8 *p = g_FastWrite[a >> pimac::cpu::kFastPageShift];
	if (p && off < kFastOffsetMask) {
		p += off;
		p[0] = (u8)(value >> 8);
		p[1] = (u8)value;
		return;
	}
	g_Bus->BusWrite16(a, value & 0xFFFF);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
	unsigned int second;
	pimac_fault_data = value;
	const unsigned int first = Phys32(second, address, true);
	const unsigned int off = first & kFastOffsetMask;
	u8 *p = g_FastWrite[first >> pimac::cpu::kFastPageShift];
	if (p && second == first + 2 && off <= kFastOffsetMask - 3) {
		p += off;
		p[0] = (u8)(value >> 24);
		p[1] = (u8)(value >> 16);
		p[2] = (u8)(value >> 8);
		p[3] = (u8)value;
		return;
	}
	g_Bus->BusWrite16(first, value >> 16);
	g_Bus->BusWrite16(second, value & 0xFFFF);
}

// Instruction stream (M68K_SEPARATE_READS): through the fetch cache.
unsigned int m68k_read_immediate_16(unsigned int address)
{
	const unsigned int off = address - g_FetchPage;
	if (off < g_FetchMask) // also false for the last byte: the word would cross
		return ((unsigned int)g_FetchHost[off] << 8) | g_FetchHost[off + 1];
	return FetchSlow16(address);
}

unsigned int m68k_read_immediate_32(unsigned int address)
{
	const unsigned int off = address - g_FetchPage;
	if (off + 3 <= g_FetchMask && off < g_FetchMask) {
		const u8 *p = g_FetchHost + off;
		return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
	}
	// As before (a fault reports a long access).
	return m68k_read_memory_32(address);
}

// PC-relative operands are data as before (Musashi leaves them unmasked
// when reads are separate).
unsigned int m68k_read_pcrelative_8(unsigned int address)
{
	return m68k_read_memory_8(address & g_AddressMask);
}

unsigned int m68k_read_pcrelative_16(unsigned int address)
{
	return m68k_read_memory_16(address & g_AddressMask);
}

unsigned int m68k_read_pcrelative_32(unsigned int address)
{
	return m68k_read_memory_32(address & g_AddressMask);
}

} // extern "C"

namespace pimac {
namespace cpu {

namespace {
unsigned g_SpeedPercent = 100;
unsigned g_SpeedRemainder; // CPU cycles (x100) not yet counted as machine cycles
} // namespace

void InvalidateFetch()
{
	g_FetchPage = 1;
	g_FetchMask = 0;
}

void ClearFastMap()
{
	InvalidateFetch();
	memset(g_FastRead, 0, kFastPages * sizeof *g_FastRead);
	memset(g_FastWrite, 0, kFastPages * sizeof *g_FastWrite);
}

void MapFastPage(u32 address, const u8 *read, u8 *write)
{
	InvalidateFetch();
	g_FastRead[address >> kFastPageShift] = read;
	g_FastWrite[address >> kFastPageShift] = write;
}

void Init(CpuBus *bus)
{
	if (!g_FastRead) {
		g_FastRead = new const u8 *[kFastPages];
		g_FastWrite = new u8 *[kFastPages];
	}
	ClearFastMap();
	g_Bus = bus;
	g_SpeedPercent = 100;
	g_SpeedRemainder = 0;
	mmu::Attach(bus);
	m68k_init();
	pimac_fpu_present = 0;
	SetType(M68K_CPU_TYPE_68000);
	Reset();
}

void SetType(unsigned musashiType)
{
	m68k_set_cpu_type(musashiType);
	g_AddressMask = musashiType == M68K_CPU_TYPE_68000 || musashiType == M68K_CPU_TYPE_68010 ||
				musashiType == M68K_CPU_TYPE_68EC020
			? 0x00FFFFFF : 0xFFFFFFFF;
	InvalidateFetch();
}

void SetRollback(bool on)
{
	m68k_set_rollback(on ? 1 : 0);
}

void SetFpu(bool present)
{
	pimac_fpu_present = present ? 1 : 0;
	pimac_fpu_reset();
}

void Reset()
{
	mmu::Reset();
	pimac_fpu_reset();
	m68k_pulse_reset();
}

void SetSpeedPercent(unsigned percent)
{
	g_SpeedPercent = percent < 100 ? 100 : percent > 400 ? 400 : percent;
	g_SpeedRemainder = 0;
}

int Execute(int cycles)
{
	if (g_SpeedPercent == 100)
		return m68k_execute(cycles);
	const int ran = m68k_execute((int)((unsigned)cycles * g_SpeedPercent / 100));
	if (ran <= 0)
		return ran;
	// Convert back, carrying the fraction so no time is lost over many slices.
	const unsigned scaled = (unsigned)ran * 100 + g_SpeedRemainder;
	g_SpeedRemainder = scaled % g_SpeedPercent;
	return (int)(scaled / g_SpeedPercent);
}

int CyclesRun()
{
	return (int)((unsigned)m68k_cycles_run() * 100 / g_SpeedPercent);
}

void EndTimeslice()
{
	m68k_end_timeslice();
}

void SetIrq(unsigned level)
{
	m68k_set_irq(level);
}

u32 PC() { return m68k_get_reg(0, M68K_REG_PC); }
u32 SP() { return m68k_get_reg(0, M68K_REG_SP); }
u32 SR() { return m68k_get_reg(0, M68K_REG_SR); }
u32 D(unsigned n) { return m68k_get_reg(0, (m68k_register_t)(M68K_REG_D0 + (n & 7))); }
u32 A(unsigned n) { return m68k_get_reg(0, (m68k_register_t)(M68K_REG_A0 + (n & 7))); }

} // namespace cpu
} // namespace pimac
