#include "mac/cpu.h"

extern "C" {
#include "m68k.h"
}

namespace {
pimac::CpuBus *g_Bus = 0;
}

// Musashi memory callbacks. 32-bit accesses are split into two 16-bit bus
// cycles, as on the real 68000.
extern "C" {

unsigned int m68k_read_memory_8(unsigned int address)
{
	return g_Bus->BusRead8(address);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
	return g_Bus->BusRead16(address);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
	unsigned int hi = g_Bus->BusRead16(address);
	return (hi << 16) | g_Bus->BusRead16(address + 2);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
	g_Bus->BusWrite8(address, value & 0xFF);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
	g_Bus->BusWrite16(address, value & 0xFFFF);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
	g_Bus->BusWrite16(address, value >> 16);
	g_Bus->BusWrite16(address + 2, value & 0xFFFF);
}

} // extern "C"

namespace pimac {
namespace cpu {

void Init(CpuBus *bus)
{
	g_Bus = bus;
	m68k_init();
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	Reset();
}

void Reset()
{
	m68k_pulse_reset();
}

int Execute(int cycles)
{
	return m68k_execute(cycles);
}

int CyclesRun()
{
	return m68k_cycles_run();
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
