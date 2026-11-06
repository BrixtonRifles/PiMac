#include "mac/mac_se30.h"

#include "core/log.h"
#include "mac/mmu030.h"

extern "C" {
#include "m68k.h"
}

namespace pimac {

namespace {

const unsigned kSoundChunkLines = 10;
const u32 kRomOverlaySize = 0x40000;
const u8 kSlotE = 0x20; // VIA2 PA5

} // namespace

MacSE30::MacSE30()
	: m_Ram(0), m_RamSize(0), m_Vram(0), m_RomCopy(0), m_HaveVideoRom(false), m_Rom(0), m_RomMask(0),
	  m_Overlay(true), m_Running(false), m_Via2Wiring(*this), m_Via1(this, 20),
	  m_Via2(&m_Via2Wiring, 20), m_Via1Irq(false), m_Via2Irq(false), m_Ipl(0), m_AdbState(3),
	  m_MainScreen(true), m_VblEnabled(false), m_Glue(0), m_SlotIrqs(0xFF), m_Pb7Pin(true),
	  m_Pb7Toggles(0), m_AscIrq(false), m_Cycles(0), m_InSlice(false), m_FrameStart(0),
	  m_NextVbl(0), m_VblEnd(0), m_NextSecond(0), m_SoundLine(0), m_Input(m_Adb)
{
	m_Error[0] = '\0';
	memset(m_Page, 0, sizeof m_Page);
	memset(m_VideoRom, 0, sizeof m_VideoRom);
	memset(m_Sound, 0, sizeof m_Sound);
}

MacSE30::~MacSE30()
{
	delete[] m_Ram;
	delete[] m_Vram;
	delete[] m_RomCopy;
}

void MacSE30::SetVideoRom(const u8 *data, unsigned size)
{
	m_HaveVideoRom = data && size == kVideoRomSize;
	if (m_HaveVideoRom)
		memcpy(m_VideoRom, data, kVideoRomSize);
}

bool MacSE30::Start(const MacConfig &config)
{
	m_Running = false;
	static const unsigned kSizes[] = {4096, 5120, 8192, 16384, 20480, 32768, 65536, 131072};
	bool sizeOk = false;
	for (unsigned i = 0; i < sizeof kSizes / sizeof kSizes[0]; ++i)
		sizeOk = sizeOk || config.ramKB == kSizes[i];
	if (!sizeOk) {
		StrCopy(m_Error, "RAM must be 4, 5, 8, 16, 20, 32, 64 or 128 MB", sizeof m_Error);
		return false;
	}
	if (!config.rom || config.romSize < kRomOverlaySize || (config.romSize & (config.romSize - 1))) {
		StrCopy(m_Error, "ROM image missing or bad size", sizeof m_Error);
		return false;
	}
	if (!m_HaveVideoRom) {
		StrCopy(m_Error, "needs the SE/30 video ROM (se30vrom)", sizeof m_Error);
		return false;
	}
	if (m_RamSize != config.ramKB * 1024) {
		delete[] m_Ram;
		m_RamSize = config.ramKB * 1024;
		m_Ram = new u8[m_RamSize];
	}
	if (!m_Vram)
		m_Vram = new u8[kVramSize];
	// A copy with the FPU bit of HwCfgFlags cleared (see the header).
	delete[] m_RomCopy;
	m_RomCopy = new u8[config.romSize];
	memcpy(m_RomCopy, config.rom, config.romSize);
	static const u8 kSetHwCfg[6] = {0x31, 0xFC, 0xDC, 0x00, 0x0B, 0x22}; // move.w #$DC00,$0B22
	if (config.romSize > 0x330 && !memcmp(m_RomCopy + 0x32A, kSetHwCfg, sizeof kSetHwCfg)) {
		m_RomCopy[0x32C] = 0xCC;
		u32 sum = 0; // the ROM's checksum: the 16-bit words after the first 4 bytes
		for (u32 i = 4; i < config.romSize; i += 2)
			sum += ((u32)m_RomCopy[i] << 8) | m_RomCopy[i + 1];
		m_RomCopy[0] = (u8)(sum >> 24);
		m_RomCopy[1] = (u8)(sum >> 16);
		m_RomCopy[2] = (u8)(sum >> 8);
		m_RomCopy[3] = (u8)sum;
	}
	m_Rom = m_RomCopy;
	m_RomMask = config.romSize - 1;

	for (unsigned id = 0; id < Scsi5380::kTargets; ++id)
		m_Scsi.Attach(id, 0);
	if (config.disk)
		m_Scsi.Attach(0, config.disk);
	m_Scsi.AttachCdrom(3);
	m_Rtc.SetSeconds(config.clockSeconds);

	cpu::Init(this);
	cpu::SetType(M68K_CPU_TYPE_68030);
	cpu::SetSpeedPercent(config.cpuPercent);
	Reset();
	m_Running = true;
	return true;
}

void MacSE30::Reset()
{
	memset(m_Ram, 0, m_RamSize);
	memset(m_Vram, 0, kVramSize);
	memset(m_Page, 0, sizeof m_Page);
	m_Overlay = true;
	m_Glue = 0;
	UpdateFastMap();
	m_SlotIrqs = 0xFF;
	m_MainScreen = true;
	m_VblEnabled = false;
	m_Via1Irq = m_Via2Irq = false;
	m_AdbState = 3;
	m_Via1.Reset();
	m_Via2.Reset();
	m_Pb7Pin = true;
	m_Pb7Toggles = m_Via2.Pb7Toggles();
	m_Rtc.Reset();
	m_Adb.Reset();
	m_Scsi.Reset();
	m_Iwm.Reset();
	m_Scc.Reset();
	m_Asc.Reset();
	m_AscIrq = false;
	memset(m_Sound, 0, sizeof m_Sound);

	m_FrameStart = m_Cycles;
	m_NextVbl = m_FrameStart + (u64)kHeight * kCyclesPerLine;
	m_VblEnd = m_FrameStart + kCyclesPerFrame;
	m_NextSecond = m_Cycles + kCpuHz;
	m_SoundLine = 0;
	m_Ipl = 0;
	cpu::SetIrq(0);
	cpu::Reset();
}

u64 MacSE30::Now() const
{
	return m_InSlice ? m_Cycles + (u64)cpu::CyclesRun() : m_Cycles;
}

// --- RAM (GLUE) -------------------------------------------------------------

void MacSE30::MapRam(u8 glue)
{
	// As MAME's macii_state::via2_out_a. The RAM is banks A and B; VIA2
	// PA6-7 put bank B at 1, 2, 8 or 32 MB while the ROM sizes the RAM, and
	// the sizes it accepts need particular mirrors to pass its tests.
	memset(m_Page, 0, sizeof m_Page);
	const u32 memsize = m_RamSize;
	u32 asize = memsize >= 0x4000000 ? 0x4000000 : memsize;
	u32 bsize = memsize >= 0x4000000 ? memsize - 0x4000000 : 0;
	static const u32 kBLocation[4] = {0x100000, 0x200000, 0x800000, 0x2000000};
	const u32 blocation = kBLocation[glue >> 6];
	bool noMirror = false, mirrorB = false;
	switch (memsize >> 20) {
	case 1: case 16: noMirror = true; break;
	case 2: asize = bsize = 0x100000; break;
	case 4: noMirror = true; asize = bsize = 0x200000; break;
	case 5: asize = 0x400000; bsize = 0x100000; mirrorB = true; break;
	case 8: asize = bsize = 0x400000; break;
	case 17: asize = 0x1000000; bsize = 0x100000; mirrorB = true; break;
	case 20: asize = 0x1000000; bsize = 0x400000; mirrorB = true; break;
	case 65: asize = 0x4000000; bsize = 0x100000; mirrorB = true; break;
	case 68: asize = 0x4000000; bsize = 0x400000; mirrorB = true; break;
	case 80: asize = 0x4000000; bsize = 0x1000000; mirrorB = true; break;
	case 128: asize = bsize = 0x4000000; break;
	default: break;
	}
	const u32 page = 1u << kPageShift;
	struct Local {
		static void Map(u8 **pages, u32 at, u32 size, u8 *src)
		{
			for (u32 a = 0; a < size; a += 1u << kPageShift)
				if (((at + a) >> kPageShift) < kPages)
					pages[(at + a) >> kPageShift] = src + a;
		}
	};
	(void)page;
	if (blocation <= memsize) {
		Local::Map(m_Page, 0, memsize, m_Ram);
		if (!noMirror) {
			if (mirrorB)
				Local::Map(m_Page, memsize, bsize, m_Ram + asize);
			else
				Local::Map(m_Page, memsize, asize, m_Ram);
		}
		if (bsize && blocation >= memsize + asize)
			Local::Map(m_Page, blocation, bsize, m_Ram + asize);
	} else {
		Local::Map(m_Page, 0, 0x100000, m_Ram); // the ROM wants 1 MB at 0 while it sizes
	}
	UpdateFastMap();
}

// The CPU's fast map (cpu.h): the ROM (mirrored), and once the overlay is
// off the RAM and video RAM. The video ROM and I/O stay on the bus.
void MacSE30::UpdateFastMap()
{
	cpu::ClearFastMap();
	const u32 page = cpu::kFastPageSize;
	for (u32 a = 0; a < 0x10000000; a += page)
		cpu::MapFastPage(0x40000000 + a, m_Rom + (a & m_RomMask), 0);
	if (m_Overlay)
		return;
	for (u32 a = 0; a < (u32)kPages << kPageShift; a += page)
		if (u8 *p = m_Page[a >> kPageShift])
			cpu::MapFastPage(a, p, p);
	cpu::MapFastPage(0xFE000000, m_Vram, m_Vram);
	for (u32 a = 0xFEE00000; a < 0xFEF00000; a += page)
		cpu::MapFastPage(a, m_Vram, m_Vram);
}

void MacSE30::SetOverlay(bool on)
{
	if (on == m_Overlay)
		return;
	m_Overlay = on;
	if (!on) {
		m_Glue = 0;
		MapRam(0);
	} else {
		UpdateFastMap();
	}
}

inline u8 *MacSE30::RamAt(u32 a) const
{
	if (a >= (u32)kPages << kPageShift)
		return 0;
	u8 *p = m_Page[a >> kPageShift];
	return p ? p + (a & ((1u << kPageShift) - 1)) : 0;
}

// --- Bus --------------------------------------------------------------------

u32 MacSE30::BusRead8(u32 a)
{
	switch (a >> 28) {
	case 0: case 1: case 2: case 3: {
		if (m_Overlay && a < kRomOverlaySize)
			return m_Rom[a & m_RomMask];
		const u8 *p = RamAt(a);
		return p ? *p : 0;
	}
	case 4:
		return m_Rom[a & m_RomMask];
	case 5:
		return IoRead(a & 0xFFFFF);
	case 0xF:
		if ((a >> 16) == 0xFE00 || (a >= 0xFEE00000 && a < 0xFEF00000))
			return m_Vram[a & (kVramSize - 1)];
		if (a >= 0xFEFFE000)
			return m_VideoRom[a & (kVideoRomSize - 1)];
		return 0;
	default:
		return 0;
	}
}

u32 MacSE30::BusRead16(u32 a)
{
	switch (a >> 28) {
	case 0: case 1: case 2: case 3: {
		if (m_Overlay && a < kRomOverlaySize)
			return ((u32)m_Rom[a & m_RomMask] << 8) | m_Rom[(a + 1) & m_RomMask];
		const u8 *p = RamAt(a);
		return p ? ((u32)p[0] << 8) | p[1] : 0;
	}
	case 4:
		return ((u32)m_Rom[a & m_RomMask] << 8) | m_Rom[(a + 1) & m_RomMask];
	case 5: {
		const u32 off = a & 0xFFFFF;
		if ((off >= 0x6000 && off < 0x8000) || (off >= 0x12000 && off < 0x14000))
			return ScsiDmaRead(2);
		if (off >= 0x14000 && off < 0x16000) { // the ASC answers on every byte lane
			const u32 hi = m_Asc.Read(off & 0xFFF), lo = m_Asc.Read((off + 1) & 0xFFF);
			CheckAscIrq();
			return (hi << 8) | lo;
		}
		const u8 v = IoRead(off);
		return ((u32)v << 8) | v;
	}
	case 0xF:
		return (BusRead8(a) << 8) | BusRead8(a + 1);
	default:
		return 0;
	}
}

void MacSE30::BusWrite8(u32 a, u32 v)
{
	switch (a >> 28) {
	case 0: case 1: case 2: case 3:
		if (u8 *p = RamAt(a))
			*p = (u8)v;
		return;
	case 5:
		IoWrite(a & 0xFFFFF, (u8)v);
		return;
	case 0xF:
		if ((a >> 16) == 0xFE00 || (a >= 0xFEE00000 && a < 0xFEF00000))
			m_Vram[a & (kVramSize - 1)] = (u8)v;
		return;
	default:
		return;
	}
}

void MacSE30::BusWrite16(u32 a, u32 v)
{
	switch (a >> 28) {
	case 0: case 1: case 2: case 3:
		if (u8 *p = RamAt(a)) {
			p[0] = (u8)(v >> 8);
			p[1] = (u8)v;
		}
		return;
	case 5: {
		const u32 off = a & 0xFFFFF;
		if ((off >= 0x6000 && off < 0x8000) || (off >= 0x12000 && off < 0x14000)) {
			ScsiDmaWrite(v, 2);
			return;
		}
		if (off >= 0x14000 && off < 0x16000) { // every byte reaches the ASC
			m_Asc.Write(off & 0xFFF, (u8)(v >> 8));
			m_Asc.Write((off + 1) & 0xFFF, (u8)v);
			CheckAscIrq();
			return;
		}
		if (off >= 0x16000 && off < 0x18000)
			IoWrite(off, (u8)v); // SWIM takes the low byte
		else
			IoWrite(off, (u8)(v >> 8));
		return;
	}
	case 0xF:
		BusWrite8(a, v >> 8);
		BusWrite8(a + 1, v & 0xFF);
		return;
	default:
		return;
	}
}

u32 MacSE30::ScsiDmaRead(unsigned bytes)
{
	u32 v = 0;
	for (unsigned i = 0; i < bytes; ++i)
		v = (v << 8) | m_Scsi.Read(6, true);
	return v;
}

void MacSE30::ScsiDmaWrite(u32 value, unsigned bytes)
{
	for (unsigned i = bytes; i-- > 0;)
		m_Scsi.Write(0, (u8)(value >> (8 * i)), true);
}

u8 MacSE30::IoRead(u32 a)
{
	if (a < 0x2000 || (a >= 0x40000 && a < 0x42000))
		return m_Via1.Read((a >> 9) & 0xF, Now());
	if (a < 0x4000)
		return m_Via2.Read((a >> 9) & 0xF, Now());
	if (a < 0x6000) {
		const u8 v = m_Scc.Read((a >> 1) & 1, (a & 4) != 0, Now());
		UpdateIpl();
		return v;
	}
	if (a < 0x8000 || (a >= 0x12000 && a < 0x14000))
		return (u8)ScsiDmaRead(1);
	if (a >= 0x10000 && a < 0x12000)
		return m_Scsi.Read((a >> 4) & 7, (a & 0x200) != 0);
	if (a >= 0x14000 && a < 0x16000) {
		const u8 v = m_Asc.Read(a & 0xFFF);
		CheckAscIrq();
		return v;
	}
	if (a >= 0x16000 && a < 0x18000)
		return m_Iwm.Access((a >> 9) & 0xF, false, 0, Now());
	LogHex("SE/30 read unmapped", 0x50000000 | a, cpu::PC());
	return 0;
}

void MacSE30::IoWrite(u32 a, u8 value)
{
	if (a < 0x2000 || (a >= 0x40000 && a < 0x42000)) {
		m_Via1.Write((a >> 9) & 0xF, value, Now());
		return;
	}
	if (a < 0x4000) {
		m_Via2.Write((a >> 9) & 0xF, value, Now());
		CheckPb7();
		return;
	}
	if (a < 0x6000) {
		m_Scc.Write((a >> 1) & 1, (a & 4) != 0, value, Now());
		UpdateIpl();
		return;
	}
	if (a < 0x8000 || (a >= 0x12000 && a < 0x14000)) {
		ScsiDmaWrite(value, 1);
		return;
	}
	if (a >= 0x10000 && a < 0x12000) {
		m_Scsi.Write((a >> 4) & 7, value, (a & 0x200) != 0);
		return;
	}
	if (a >= 0x14000 && a < 0x16000) {
		m_Asc.Write(a & 0xFFF, value);
		CheckAscIrq();
		return;
	}
	if (a >= 0x16000 && a < 0x18000) {
		m_Iwm.Access((a >> 9) & 0xF, true, value, Now());
		return;
	}
	LogHex("SE/30 write unmapped", 0x50000000 | a, value);
}

// --- VIA1 -------------------------------------------------------------------

u8 MacSE30::ViaInputA()
{
	return 0x81 | 0x40; // model ID bits: PA6 set for the IIcx and SE/30
}

u8 MacSE30::ViaInputB()
{
	return (u8)((m_Rtc.DataBit() ? 0x01 : 0) | (m_Adb.InterruptLineHigh() ? 0x08 : 0));
}

void MacSE30::ViaOutputA(u8 ora, u8 ddra)
{
	const u8 pins = (u8)((ora & ddra) | ~ddra);
	m_MainScreen = (pins & 0x40) != 0;
	m_Iwm.SetSel((pins & 0x20) != 0);
	SetOverlay((pins & 0x10) != 0);
}

void MacSE30::ViaOutputB(u8 orb, u8 ddrb)
{
	const u8 pins = (u8)((orb & ddrb) | ~ddrb);
	m_Rtc.PinsChanged(pins & 0x07, (ddrb & 0x01) != 0);
	m_VblEnabled = (pins & 0x40) == 0; // PB6 low: video interrupts on
	if (!m_VblEnabled)
		SetSlotE(false);
	const unsigned state = (pins >> 4) & 3;
	if (state != m_AdbState) {
		m_AdbState = state;
		m_Adb.StateLinesChanged(state, Now());
		if (m_InSlice)
			cpu::EndTimeslice();
	}
}

void MacSE30::ViaIrq(bool asserted)
{
	m_Via1Irq = asserted;
	UpdateIpl();
}

void MacSE30::ViaTimingChanged()
{
	if (m_InSlice)
		cpu::EndTimeslice();
}

// --- VIA2 -------------------------------------------------------------------

u8 MacSE30::Via2Wiring::ViaInputA()
{
	return (u8)(m_Mac.m_Glue | m_Mac.m_SlotIrqs);
}

u8 MacSE30::Via2Wiring::ViaInputB()
{
	// The ROM's model test (ROM 0x3F768): VIA1 PA6 set + VIA2 PB3 clear =
	// SE/30 (BoxFlag 3); PB3 set would make it a IIcx (MAME's 0xCF).
	return 0xC7;
}

void MacSE30::Via2Wiring::ViaOutputA(u8 ora, u8 ddra)
{
	const u8 pins = (u8)((ora & ddra) | ~ddra);
	if ((pins & 0xC0) == m_Mac.m_Glue)
		return; // already mapped this way (port A also carries the slot interrupts)
	m_Mac.m_Glue = pins & 0xC0;
	if (!m_Mac.m_Overlay)
		m_Mac.MapRam(m_Mac.m_Glue);
}

void MacSE30::Via2Wiring::ViaOutputB(u8, u8)
{
	m_Mac.CheckPb7();
}

void MacSE30::Via2Wiring::ViaIrq(bool asserted)
{
	m_Mac.m_Via2Irq = asserted;
	m_Mac.UpdateIpl();
}

void MacSE30::Via2Wiring::ViaTimingChanged()
{
	if (m_Mac.m_InSlice)
		cpu::EndTimeslice();
}

void MacSE30::CheckPb7()
{
	// VIA2 PB7 drives VIA1 CA1 (the 60.15 Hz tick): from timer 1 when ACR
	// bit 7 is set (how the ROM makes it), else the port B output.
	const u32 toggles = m_Via2.Pb7Toggles();
	u32 n = toggles - m_Pb7Toggles;
	m_Pb7Toggles = toggles;
	if (!m_Via2.Pb7TimerOutput() || !n)
		return;
	if (n > 2)
		n = 2 - (n & 1); // replay at most one cycle, ending at the right level
	while (n--) {
		m_Pb7Pin = !m_Pb7Pin;
		Tick60();
	}
	m_Pb7Pin = m_Via2.Pb7();
}

void MacSE30::Tick60()
{
	// CA1 interrupts on the edge PCR bit 0 selects (1 = rising).
	const bool rising = (m_Via1.Pcr() & 0x01) != 0;
	if (m_Pb7Pin == rising)
		m_Via1.SignalCA1();
}

void MacSE30::SetSlotE(bool asserted)
{
	if (asserted) {
		m_SlotIrqs &= (u8)~kSlotE;
		m_Via2.SignalCA1(); // any slot interrupt: a falling edge on CA1
	} else {
		m_SlotIrqs |= kSlotE;
	}
}

void MacSE30::CheckAscIrq()
{
	const bool irq = m_Asc.Irq();
	if (irq && !m_AscIrq)
		m_Via2.SignalCB1();
	m_AscIrq = irq;
}

void MacSE30::UpdateIpl()
{
	unsigned ipl = 0;
	if (m_Scc.Irq())
		ipl = 4;
	else if (m_Via2Irq)
		ipl = 2;
	else if (m_Via1Irq)
		ipl = 1;
	if (ipl != m_Ipl) {
		m_Ipl = ipl;
		cpu::SetIrq(ipl);
	}
}

// --- Frame loop ---------------------------------------------------------------

void MacSE30::ServiceEvents()
{
	const u64 now = m_Cycles;
	m_Via1.Update(now);
	m_Via2.Update(now);
	CheckPb7();
	m_Adb.Update(now, m_Via1);
	m_Scc.Update(now);

	const unsigned line = (unsigned)((now - m_FrameStart) / kCyclesPerLine);
	while (m_SoundLine < kLines && m_SoundLine <= line)
		m_Sound[m_SoundLine++] = m_Asc.NextSample();
	CheckAscIrq();

	if (now >= m_NextVbl) {
		if (m_VblEnabled)
			SetSlotE(true);
		m_Adb.Poll(m_Via1);
		m_NextVbl += kCyclesPerFrame;
	}
	if (now >= m_NextSecond) {
		m_Rtc.Tick();
		m_Via1.SignalCA2();
		m_NextSecond += kCpuHz;
	}
	UpdateIpl();
}

void MacSE30::RunFrame()
{
	if (!m_Running)
		return;

	// Virtual memory: as MacV8::RunFrame.
	if (m_Rtc.Pram()[0x0B] & 0x20) {
		mmu::TakeDisabled();
		cpu::SetRollback(true);
	} else if (mmu::TakeDisabled()) {
		cpu::SetRollback(false);
	}

	const u64 frameEnd = m_FrameStart + kCyclesPerFrame;
	while (m_Cycles < frameEnd) {
		u64 target = frameEnd;
		u64 e = m_Via1.NextEvent();
		if (e < target) target = e;
		e = m_Via2.NextEvent();
		if (e < target) target = e;
		e = m_Adb.NextEvent();
		if (e < target) target = e;
		e = m_Scc.NextEvent();
		if (e < target) target = e;
		if (m_NextVbl < target) target = m_NextVbl;
		if (m_NextSecond < target) target = m_NextSecond;
		const u64 sound = m_FrameStart + (u64)(m_SoundLine + kSoundChunkLines) * kCyclesPerLine;
		if (sound < target) target = sound;
		if (m_Cycles + kMaxSlice < target) target = m_Cycles + kMaxSlice;

		const int n = target > m_Cycles ? (int)(target - m_Cycles) : 1;
		m_InSlice = true;
		const int ran = cpu::Execute(n);
		m_InSlice = false;
		m_Cycles += ran > 0 ? (u64)ran : 1;

		ServiceEvents();
	}
	while (m_SoundLine < kLines)
		m_Sound[m_SoundLine++] = m_Asc.NextSample();
	CheckAscIrq();
	m_SoundLine = 0;
	m_FrameStart = frameEnd;
}

unsigned MacSE30::SoundFrame(s16 *out, unsigned max, u32 &rateMilliHz)
{
	rateMilliHz = kSoundMilliHz;
	const unsigned n = max < (unsigned)kLines ? max : (unsigned)kLines;
	memcpy(out, m_Sound, n * sizeof(s16));
	return n;
}

MonoFrame MacSE30::Screen() const
{
	MonoFrame f;
	f.bits = m_Vram + (m_MainScreen ? 0x8000 : 0) + kRowBytes;
	f.width = kWidth;
	f.height = kHeight;
	f.rowBytes = kRowBytes;
	return f;
}

// --- Input ------------------------------------------------------------------


} // namespace pimac
