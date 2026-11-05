#include "mac/cpu.h"

#include "mac/mmu030.h"

extern "C" {
#include "m68k.h"
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

} // extern "C"

namespace pimac {
namespace cpu {

namespace {
unsigned g_SpeedPercent = 100;
unsigned g_SpeedRemainder; // CPU cycles (x100) not yet counted as machine cycles
} // namespace

void ClearFastMap()
{
	memset(g_FastRead, 0, kFastPages * sizeof *g_FastRead);
	memset(g_FastWrite, 0, kFastPages * sizeof *g_FastWrite);
}

void MapFastPage(u32 address, const u8 *read, u8 *write)
{
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
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	Reset();
}

void SetType(unsigned musashiType)
{
	m68k_set_cpu_type(musashiType);
}

void SetRollback(bool on)
{
	m68k_set_rollback(on ? 1 : 0);
}

void Reset()
{
	mmu::Reset();
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
