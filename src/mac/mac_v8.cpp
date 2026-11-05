#include "mac/mac_v8.h"

#include "core/log.h"
#include "mac/mmu030.h"

extern "C" {
#include "m68k.h"
}

namespace pimac {

namespace {

u8 AdbFromMacKey(u8 macKey)
{
	switch (macKey) {
	case 0x3B: return 0x36; // Control
	case 0x7B: return 0x3B; // Left
	case 0x7C: return 0x3C; // Right
	case 0x7D: return 0x3D; // Down
	case 0x7E: return 0x3E; // Up
	default: return macKey;
	}
}

const u8 kAdbCapsLock = 0x39;
const unsigned kSoundChunkLines = 10; // sample the sound FIFO this often

} // namespace

MacV8::MacV8(Model model)
	: m_Model(model), m_Spice(model != ModelClassic2), m_Sonora(model == ModelColorClassic2), m_MbRam(0),
	  m_MbSize(model != ModelClassic2 ? 0x400000 : 0x200000), m_SimmRam(0), m_SimmSize(0), m_Rom(0), m_RomMask(0), m_Overlay(true),
	  m_Running(false), m_Via(this, 20), m_Egret(m_Via, m_Adb, m_Rtc, kCpuHz / 1000),
	  m_Asc(model != ModelClassic2), m_RamConfig(0), m_Vram(0), m_PaletteSerial(0),
	  m_ArielAddress(0), m_ArielRgb(0), m_ArielControl(0), m_ArielKey(0),
	  m_VblOffset(model != ModelClassic2 ? kColorHeight * 640 : kHeight * kCyclesPerLine),
	  m_VramSize(model == ModelColorClassic2 ? kSonoraVramSize : kVramSize),
	  m_VMode(0x9F), m_VDepth(0), m_MonitorId(8), m_VTest(0),
	  m_ViaIrq(false), m_PortBPins(0xFF), m_Ipl(0),
	  m_Cycles(0), m_InSlice(false), m_FrameStart(0), m_NextVbl(0), m_SoundLine(0),
	  m_CapsLocked(false)
{
	m_Error[0] = '\0';
	memset(m_Page, 0, sizeof m_Page);
	memset(m_PvRegs, 0, sizeof m_PvRegs);
	memset(m_Sound, 0, sizeof m_Sound);
	memset(m_KeysDown, 0, sizeof m_KeysDown);
	memset(m_Palette, 0, sizeof m_Palette);
	memset(m_PaletteRam, 0, sizeof m_PaletteRam);
	m_Egret.SetCuda(m_Spice);
}

MacV8::~MacV8()
{
	delete[] m_MbRam;
	delete[] m_SimmRam;
	delete[] m_Vram;
}

bool MacV8::Start(const MacConfig &config)
{
	m_Running = false;
	if (m_Sonora) {
		const u32 simm = config.ramKB - 4096;
		if (config.ramKB < 4096 || (simm && (simm & (simm - 1))) || simm == 1024 || simm > 32768) {
			StrCopy(m_Error, "RAM must be 4 MB + a 2-32 MB SIMM", sizeof m_Error);
			return false;
		}
	} else if (m_Spice ? (config.ramKB != 4096 && config.ramKB != 6144 && config.ramKB != 8192 &&
		       config.ramKB != 10240)
		    : (config.ramKB != 2048 && config.ramKB != 4096 && config.ramKB != 6144 &&
		       config.ramKB != 10240)) {
		StrCopy(m_Error, m_Spice ? "RAM must be 4, 6, 8 or 10 MB" : "RAM must be 2, 4, 6 or 10 MB",
			sizeof m_Error);
		return false;
	}
	if (!config.rom || config.romSize < 0x80000 || (config.romSize & (config.romSize - 1))) {
		StrCopy(m_Error, "ROM image missing or bad size", sizeof m_Error);
		return false;
	}
	delete[] m_MbRam;
	delete[] m_SimmRam;
	m_MbRam = new u8[m_MbSize];
	m_SimmSize = config.ramKB * 1024 - m_MbSize; // 0, 2, 4 or 8 MB of SIMMs
	if (m_Spice && !m_Sonora && config.ramKB == 10240)
		m_SimmSize = 0x800000; // 8 MB SIMM: 2 MB of the board RAM unused
	m_SimmRam = m_SimmSize ? new u8[m_SimmSize] : 0;
	if (m_Spice && !m_Vram)
		m_Vram = new u8[m_VramSize];
	m_Rom = config.rom;
	m_RomMask = config.romSize - 1;

	for (unsigned id = 0; id < Scsi5380::kTargets; ++id)
		m_Scsi.Attach(id, 0);
	if (config.disk)
		m_Scsi.Attach(0, config.disk);
	m_Scsi.AttachCdrom(3); // empty until the host inserts a disc
	m_Rtc.SetSeconds(config.clockSeconds);

	cpu::Init(this);
	cpu::SetType(M68K_CPU_TYPE_68030);
	cpu::SetSpeedPercent(m_Sonora ? config.cpuPercent * kSonoraCpuPercent / 100 : config.cpuPercent);
	Reset();
	m_Running = true;
	return true;
}

void MacV8::Reset()
{
	memset(m_MbRam, 0, m_MbSize);
	if (m_Vram)
		memset(m_Vram, 0, m_VramSize);
	m_VMode = 0x9F;
	m_VDepth = 0;
	m_MonitorId = 8;
	m_VTest = 0;
	memset(m_Palette, 0, sizeof m_Palette);
	memset(m_PaletteRam, 0, sizeof m_PaletteRam);
	++m_PaletteSerial;
	m_ArielAddress = m_ArielRgb = m_ArielControl = m_ArielKey = 0;
	if (m_SimmRam)
		memset(m_SimmRam, 0, m_SimmSize);
	m_Overlay = true;
	memset(m_Page, 0, sizeof m_Page);
	cpu::ClearFastMap();
	memset(m_PvRegs, 0, sizeof m_PvRegs);
	m_PvRegs[0x02] = 0x7F;
	m_PvRegs[0x03] = 0x1B;
	m_RamConfig = 0;
	m_ViaIrq = false;
	m_PortBPins = 0xFF;
	m_Via.Reset();
	m_Rtc.Reset();
	m_Egret.Reset();
	m_Scsi.Reset();
	m_Iwm.Reset();
	m_Scc.Reset();
	m_Asc.Reset();
	memset(m_Sound, 0, sizeof m_Sound);

	m_FrameStart = m_Cycles;
	m_NextVbl = m_FrameStart + m_VblOffset;
	m_SoundLine = 0;
	m_Ipl = 0;
	UpdateIpl();
	cpu::Reset();
}

u64 MacV8::Now() const
{
	return m_InSlice ? m_Cycles + (u64)cpu::CyclesRun() : m_Cycles;
}

// --- RAM controller ---------------------------------------------------------

void MacV8::MapRam(u8 config)
{
	// After MAME's v8_device::ram_size. Bits 6-7: SIMM size (0, 2, 4, 8 MB),
	// bit 5: use only 2 MB of the motherboard RAM. The SIMM goes at 0, the
	// motherboard RAM after it, and its first 2 MB always also appear at
	// 0x800000.
	memset(m_Page, 0, sizeof m_Page);
	const u32 page = 1u << kPageShift;
	for (u32 a = 0x800000; a < 0xA00000; a += page)
		m_Page[a >> kPageShift] = m_MbRam + (a - 0x800000);

	u32 mbAt = 0;
	if (m_SimmSize && (config & 0xC0)) {
		static const u32 kSimm[4] = {0, 0x200000, 0x400000, 0x800000};
		const u32 size = kSimm[config >> 6] < m_SimmSize ? kSimm[config >> 6] : m_SimmSize;
		for (u32 a = 0; a < size; a += page)
			m_Page[a >> kPageShift] = m_SimmRam + a;
		mbAt = kSimm[config >> 6];
	}
	if ((config & 0xC0) != 0xC0) {
		const u32 mbSize = (config & 0x20) ? 0x200000 : m_MbSize;
		for (u32 a = 0; a < mbSize && mbAt + a < 0x800000; a += page)
			m_Page[(mbAt + a) >> kPageShift] = m_MbRam + a;
	}
	UpdateFastMap();
}

void MacV8::EndOverlay()
{
	m_Overlay = false;
	if (m_Sonora) {
		// Sonora: board RAM, then the SIMM, from 0 (as MAME's sonora_device;
		// the ROM sizes it by probing).
		memset(m_Page, 0, sizeof m_Page);
		const u32 page = 1u << kPageShift;
		for (u32 a = 0; a < m_MbSize; a += page)
			m_Page[a >> kPageShift] = m_MbRam + a;
		for (u32 a = 0; a < m_SimmSize; a += page)
			m_Page[(m_MbSize + a) >> kPageShift] = m_SimmRam + a;
		UpdateFastMap();
		return;
	}
	MapRam(0xC0); // as MAME: "full SIMM" until the ROM sizes the RAM
}

// The CPU's fast map (cpu.h): RAM, ROM and video RAM, with their mirrors,
// once the overlay is off (until then a ROM access must end it).
void MacV8::UpdateFastMap()
{
	cpu::ClearFastMap();
	if (m_Overlay)
		return;
	const u32 page = cpu::kFastPageSize;
	if (m_Sonora) {
		for (u32 a = 0; a < (u32)kPages << kPageShift; a += page)
			if (u8 *p = m_Page[a >> kPageShift])
				cpu::MapFastPage(a, p, p);
		for (u32 a = 0; a < 0x10000000; a += page) {
			cpu::MapFastPage(0x40000000 + a, m_Rom + (a & m_RomMask), 0);
			u8 *v = m_Vram + (a & (kSonoraVramSize - 1));
			cpu::MapFastPage(0x60000000 + a, v, v);
		}
		return;
	}
	// 24-bit: the low 16 MB repeats up to 2 GB (bit 31 set reads nothing).
	for (u32 base = 0; base < 0x80000000; base += 0x1000000) {
		for (u32 a = 0; a < 0xA00000; a += page)
			if (u8 *p = m_Page[a >> kPageShift])
				cpu::MapFastPage(base + a, p, p);
		for (u32 a = 0xA00000; a < 0xB00000; a += page)
			cpu::MapFastPage(base + a, m_Rom + (a & m_RomMask), 0);
		if (m_Vram)
			for (u32 a = 0xF40000; a < 0xFC0000; a += page)
				cpu::MapFastPage(base + a, m_Vram + (a - 0xF40000), m_Vram + (a - 0xF40000));
	}
}

inline u8 *MacV8::RamAt(u32 a) const
{
	u8 *p = m_Page[a >> kPageShift];
	return p ? p + (a & ((1u << kPageShift) - 1)) : 0;
}

// --- Bus --------------------------------------------------------------------

u32 MacV8::BusRead8(u32 a)
{
	if (m_Sonora)
		return SonoraRead8(a);
	if (a & 0x80000000)
		return 0;
	a &= 0xFFFFFF;
	if (a < 0xA00000) {
		if (m_Overlay)
			return m_Rom[a & m_RomMask];
		const u8 *p = RamAt(a);
		return p ? *p : 0;
	}
	if (a < 0xB00000) {
		if (m_Overlay)
			EndOverlay();
		return m_Rom[a & m_RomMask];
	}
	if (m_Vram && a >= 0xF40000 && a < 0xFC0000)
		return m_Vram[a - 0xF40000];
	return IoRead(a);
}

u32 MacV8::BusRead16(u32 a)
{
	if (m_Sonora)
		return SonoraRead16(a);
	if (a & 0x80000000)
		return 0;
	a &= 0xFFFFFF;
	if (a < 0xA00000) {
		if (m_Overlay)
			return ((u32)m_Rom[a & m_RomMask] << 8) | m_Rom[(a + 1) & m_RomMask];
		const u8 *p = RamAt(a);
		return p ? ((u32)p[0] << 8) | p[1] : 0;
	}
	if (a < 0xB00000) {
		if (m_Overlay)
			EndOverlay();
		return ((u32)m_Rom[a & m_RomMask] << 8) | m_Rom[(a + 1) & m_RomMask];
	}
	if (m_Vram && a >= 0xF40000 && a < 0xFC0000) {
		const u8 *p = m_Vram + (a - 0xF40000);
		return ((u32)p[0] << 8) | p[1];
	}
	return IoRead16(a);
}

u32 MacV8::IoRead16(u32 a)
{
	if ((a >= 0xF06000 && a < 0xF08000) || (a >= 0xF12000 && a < 0xF14000))
		return ScsiDmaRead(2);
	const u8 v = IoRead(a);
	return ((u32)v << 8) | v;
}

void MacV8::BusWrite8(u32 a, u32 v)
{
	if (m_Sonora) {
		SonoraWrite8(a, v);
		return;
	}
	if (a & 0x80000000)
		return;
	a &= 0xFFFFFF;
	if (a < 0xA00000) {
		if (!m_Overlay)
			if (u8 *p = RamAt(a))
				*p = (u8)v;
		return;
	}
	if (a < 0xB00000)
		return;
	if (m_Vram && a >= 0xF40000 && a < 0xFC0000) {
		m_Vram[a - 0xF40000] = (u8)v;
		return;
	}
	IoWrite(a, (u8)v);
}

void MacV8::BusWrite16(u32 a, u32 v)
{
	if (m_Sonora) {
		SonoraWrite16(a, v);
		return;
	}
	if (a & 0x80000000)
		return;
	a &= 0xFFFFFF;
	if (a < 0xA00000) {
		if (!m_Overlay)
			if (u8 *p = RamAt(a)) {
				p[0] = (u8)(v >> 8);
				p[1] = (u8)v;
			}
		return;
	}
	if (a < 0xB00000)
		return;
	if (m_Vram && a >= 0xF40000 && a < 0xFC0000) {
		u8 *p = m_Vram + (a - 0xF40000);
		p[0] = (u8)(v >> 8);
		p[1] = (u8)v;
		return;
	}
	IoWrite16(a, v);
}

void MacV8::IoWrite16(u32 a, u32 v)
{
	if ((a >= 0xF06000 && a < 0xF08000) || (a >= 0xF12000 && a < 0xF14000)) {
		ScsiDmaWrite(v, 2);
		return;
	}
	// The ASC sits on all byte lanes: each byte of a wide write reaches it
	// (the Sound Manager fills the FIFO with move.l, four samples at a time).
	if (a >= 0xF14000 && a < 0xF16000) {
		m_Asc.Write(a & 0x1FFF, (u8)(v >> 8));
		m_Asc.Write((a + 1) & 0x1FFF, (u8)v);
		PseudoViaRecalc();
		return;
	}
	// SWIM takes the low byte of a word write; the others the high byte.
	if (a >= 0xF16000 && a < 0xF18000)
		IoWrite(a, (u8)v);
	else
		IoWrite(a, (u8)(v >> 8));
}

// --- Sonora (Color Classic II) bus ----------------------------------------------

u32 MacV8::SonoraRead8(u32 a)
{
	switch (a >> 28) {
	case 0: case 1: case 2: case 3: {
		if (m_Overlay)
			return m_Rom[a & m_RomMask];
		const u8 *p = a < (u32)kPages << kPageShift ? RamAt(a) : 0;
		return p ? *p : 0;
	}
	case 4:
		if (m_Overlay)
			EndOverlay();
		return m_Rom[a & m_RomMask];
	case 5:
		if (a >= 0x5FFFFFFC)
			return (u8)(kSonoraId >> (8 * (3 - (a & 3))));
		return IoRead(0xF00000 | (a & 0xFFFFF));
	case 6:
		return m_Vram[a & (kSonoraVramSize - 1)];
	default:
		return 0;
	}
}

u32 MacV8::SonoraRead16(u32 a)
{
	switch (a >> 28) {
	case 0: case 1: case 2: case 3: {
		if (m_Overlay)
			return ((u32)m_Rom[a & m_RomMask] << 8) | m_Rom[(a + 1) & m_RomMask];
		const u8 *p = a < (u32)kPages << kPageShift ? RamAt(a) : 0;
		return p ? ((u32)p[0] << 8) | p[1] : 0;
	}
	case 4:
		if (m_Overlay)
			EndOverlay();
		return ((u32)m_Rom[a & m_RomMask] << 8) | m_Rom[(a + 1) & m_RomMask];
	case 5:
		if (a >= 0x5FFFFFFC)
			return (a & 2) ? (kSonoraId & 0xFFFF) : (kSonoraId >> 16);
		return IoRead16(0xF00000 | (a & 0xFFFFF));
	case 6: {
		const u8 *p = m_Vram + (a & (kSonoraVramSize - 1));
		return ((u32)p[0] << 8) | p[1];
	}
	default:
		return 0;
	}
}

void MacV8::SonoraWrite8(u32 a, u32 v)
{
	switch (a >> 28) {
	case 0: case 1: case 2: case 3:
		if (!m_Overlay && a < (u32)kPages << kPageShift)
			if (u8 *p = RamAt(a))
				*p = (u8)v;
		return;
	case 5:
		if (a < 0x5FFFFFFC)
			IoWrite(0xF00000 | (a & 0xFFFFF), (u8)v);
		return;
	case 6:
		m_Vram[a & (kSonoraVramSize - 1)] = (u8)v;
		return;
	default:
		return;
	}
}

void MacV8::SonoraWrite16(u32 a, u32 v)
{
	switch (a >> 28) {
	case 0: case 1: case 2: case 3:
		if (!m_Overlay && a < (u32)kPages << kPageShift)
			if (u8 *p = RamAt(a)) {
				p[0] = (u8)(v >> 8);
				p[1] = (u8)v;
			}
		return;
	case 5:
		if (a < 0x5FFFFFFC)
			IoWrite16(0xF00000 | (a & 0xFFFFF), v);
		return;
	case 6: {
		u8 *p = m_Vram + (a & (kSonoraVramSize - 1));
		p[0] = (u8)(v >> 8);
		p[1] = (u8)v;
		return;
	}
	default:
		return;
	}
}

// Sonora video controls (MAME's mv_sonora.cpp): 0 mode (bit 7 = blanked,
// low 5 bits = modeline), 1 depth (0-4 = 1-16 bpp), 2 monitor sense (low 4
// bits: which lines the ROM drives; bits 4-6 read the lines), 3 test,
// 4-7 the beam position.
u8 MacV8::SonoraVideoRead(unsigned reg)
{
	switch (reg) {
	case 0:
		return m_VMode;
	case 1:
		return m_VDepth;
	case 2: {
		u8 sense = 2; // 12" RGB (512x384): the built-in display
		if (!(m_MonitorId & 8))
			sense &= m_MonitorId & 7;
		return (u8)(m_MonitorId | (sense << 4));
	}
	case 3:
		return m_VTest;
	default: {
		const u32 dot = (u32)((Now() - m_FrameStart) % kCyclesPerFrame);
		const u32 h = dot % 640, v = dot / 640;
		switch (reg) {
		case 4: return (u8)((h >> 8) & 7);
		case 5: return (u8)h;
		case 6: return (u8)((v >> 8) & 3);
		default: return (u8)v;
		}
	}
	}
}

void MacV8::SonoraVideoWrite(unsigned reg, u8 value)
{
	switch (reg) {
	case 0: m_VMode = value & 0x9F; break;
	case 1: m_VDepth = value & 7; break;
	case 2: m_MonitorId = value & 0xF; break;
	case 3: m_VTest = value & 1; break;
	default: break;
	}
}

u32 MacV8::ScsiDmaRead(unsigned bytes)
{
	u32 v = 0;
	for (unsigned i = 0; i < bytes; ++i)
		v = (v << 8) | m_Scsi.Read(6, true);
	return v;
}

void MacV8::ScsiDmaWrite(u32 value, unsigned bytes)
{
	for (unsigned i = bytes; i-- > 0;)
		m_Scsi.Write(0, (u8)(value >> (8 * i)), true);
}

// Reads of empty address space that the ROM makes on purpose at every
// startup (on a real Mac they bus-error and the ROM moves on; here they read
// 0, with the same result). Not logged, so they do not use up the log's
// budget (core/log.cpp) and hide a real problem.
static bool IsRomProbe(u32 a)
{
	// Search for a ROM extension above the main ROM: a 16-byte signature at
	// offset $80 of each 64 KB block.
	if (a >= 0xB00000 && a < 0xF00000 && (a & 0xFFF0) == 0x0080)
		return true;
	// Diagnostic / test ROM check (signature $AAAA5555).
	if (a >= 0xFC0000 && a < 0xFC0010)
		return true;
	// Classic II: the ROM's hardware-detection table testing for devices
	// other models have (the Color Classic maps F24000 and F40000 up).
	if ((a >= 0xF18000 && a < 0xF18040) || a == 0xF21C00 || a == 0xF24000 || a == 0xF41C00)
		return true;
	return false;
}

u8 MacV8::IoRead(u32 a)
{
	if (a >= 0xF00000 && a < 0xF02000)
		return m_Via.Read((a >> 9) & 0xF, Now());
	if (a >= 0xF04000 && a < 0xF06000) {
		const u8 v = m_Scc.Read((a >> 1) & 1, (a & 4) != 0, Now());
		UpdateIpl();
		return v;
	}
	if ((a >= 0xF06000 && a < 0xF08000) || (a >= 0xF12000 && a < 0xF14000))
		return (u8)ScsiDmaRead(1);
	if (a >= 0xF10000 && a < 0xF12000)
		return m_Scsi.Read((a >> 4) & 7, (a & 0x200) != 0);
	if (a >= 0xF14000 && a < 0xF16000) {
		const u8 v = m_Asc.Read(a & 0x1FFF);
		PseudoViaRecalc();
		return v;
	}
	if (a >= 0xF16000 && a < 0xF18000)
		return m_Iwm.Access((a >> 9) & 0xF, false, 0, Now());
	if (a >= 0xF26000 && a < 0xF28000)
		return PseudoViaRead(a & (m_Sonora ? 0x1F : 0x13));
	if (m_Spice && a >= 0xF24000 && a < 0xF26000)
		return ArielRead(a & 0x1FFF);
	if (m_Sonora && a >= 0xF28000 && a < 0xF28008)
		return SonoraVideoRead(a & 7);
	if (!IsRomProbe(a))
		LogHex("V8 read unmapped", a, cpu::PC());
	return 0;
}

void MacV8::IoWrite(u32 a, u8 value)
{
	if (a >= 0xF00000 && a < 0xF02000) {
		m_Via.Write((a >> 9) & 0xF, value, Now());
		return;
	}
	if (a >= 0xF04000 && a < 0xF06000) {
		m_Scc.Write((a >> 1) & 1, (a & 4) != 0, value, Now());
		UpdateIpl();
		return;
	}
	if ((a >= 0xF06000 && a < 0xF08000) || (a >= 0xF12000 && a < 0xF14000)) {
		ScsiDmaWrite(value, 1);
		return;
	}
	if (a >= 0xF10000 && a < 0xF12000) {
		m_Scsi.Write((a >> 4) & 7, value, (a & 0x200) != 0);
		return;
	}
	if (a >= 0xF14000 && a < 0xF16000) {
		m_Asc.Write(a & 0x1FFF, value);
		PseudoViaRecalc();
		return;
	}
	if (a >= 0xF16000 && a < 0xF18000) {
		m_Iwm.Access((a >> 9) & 0xF, true, value, Now());
		return;
	}
	if (a >= 0xF26000 && a < 0xF28000) {
		if (m_Sonora && (a & 0x1E00) == 0x0200)
			return; // Sonora port A outputs: nothing attached here
		PseudoViaWrite(a & (m_Sonora ? 0x1F : 0x13), value);
		return;
	}
	if (m_Sonora && a >= 0xF28000 && a < 0xF28008) {
		SonoraVideoWrite(a & 7, value);
		return;
	}
	if (a >= 0xF24000 && a < 0xF26000) {
		if (m_Spice)
			ArielWrite(a & 0x1FFF, value);
		return; // (the Classic II's mono display has no CLUT)
	}
	if (a >= 0xF18000 && a < 0xF1A000)
		return; // Spice/Color Classic brightness: the ROM's display code writes it on every model
	LogHex("V8 write unmapped", a, value);
}

// --- Pseudo-VIA ---------------------------------------------------------------

u8 MacV8::PseudoViaRead(unsigned reg)
{
	switch (reg) {
	case 0x00:
		return 0;
	case 0x01:
		if (m_Sonora)
			return 0; // (MAME: nothing on Sonora's config inputs)
		return (u8)(m_RamConfig | 0x04);
	case 0x10:
		if (m_Sonora) // monitor sense is in the video controls
			return (u8)(m_PvRegs[0x10] & ~0x38);
		if (m_Spice) // the built-in 12" RGB monitor's sense code
			return (u8)((m_PvRegs[0x10] & ~0x38) | (0x02 << 3));
		return (u8)(m_PvRegs[0x10] & ~0x38); // Eagle: no monitor sense
	case 0x12:
	case 0x13:
		return (u8)(m_PvRegs[reg] & 0x7F);
	default:
		return m_PvRegs[reg];
	}
}

void MacV8::PseudoViaWrite(unsigned reg, u8 value)
{
	switch (reg) {
	case 0x00:
		break; // PB3: HMMU enable on the LC's 68020; nothing here
	case 0x01:
		m_RamConfig = value;
		if (!m_Overlay && !m_Sonora)
			MapRam(value);
		break;
	case 0x04:
	case 0x05:
		if (m_Sonora)
			m_PvRegs[reg] = value; // system configuration
		break;
	case 0x02:
		m_PvRegs[0x02] |= value & 0x40; // acknowledge VBL
		break;
	case 0x03:
		m_PvRegs[0x03] &= (u8)~(value & 0x7F);
		break;
	case 0x10:
		m_PvRegs[0x10] = value;
		break;
	case 0x12:
	case 0x13:
		if (value & 0x80)
			m_PvRegs[reg] |= value & 0x7F;
		else
			m_PvRegs[reg] &= (u8)~(value & 0x7F);
		break;
	default:
		break;
	}
	PseudoViaRecalc();
}

void MacV8::PseudoViaRecalc()
{
	// The sound FIFO interrupt arrives on CB1. On V8/Eagle it is level
	// triggered (MAME's v8_pseudovia_device): the flag follows the line, so
	// an idle, half-empty FIFO keeps asking until the Sound Manager disables
	// it in the IER. (Edge-triggered, only the startup chime played.)
	if (m_Asc.Irq())
		m_PvRegs[0x03] |= 0x10;
	else
		m_PvRegs[0x03] &= (u8)~0x10;

	// Slot interrupts (active low in register 2) collapse into bit 1.
	const u8 slots = (u8)(~m_PvRegs[0x02] & 0x78 & m_PvRegs[0x12]);
	if (slots)
		m_PvRegs[0x03] |= 0x02;
	else
		m_PvRegs[0x03] &= (u8)~0x02;
	const u8 ifr = m_PvRegs[0x03] & m_PvRegs[0x13] & 0x1B;
	if (ifr)
		m_PvRegs[0x03] = (u8)(ifr | 0x80); // as MAME (disabled flags drop)
	else
		m_PvRegs[0x03] &= 0x7F;
	UpdateIpl();
}

// --- Ariel CLUT (Spice) ---------------------------------------------------------

u8 MacV8::ArielRead(unsigned reg)
{
	switch (reg) {
	case 0:
		m_ArielRgb = 0;
		return m_ArielAddress;
	case 1: {
		const u8 v = m_PaletteRam[m_ArielAddress][m_ArielRgb];
		if (++m_ArielRgb == 3) {
			m_ArielRgb = 0;
			++m_ArielAddress;
		}
		return v;
	}
	case 2:
		return m_ArielControl;
	case 3:
		return m_ArielKey;
	default:
		return 0;
	}
}

void MacV8::ArielWrite(unsigned reg, u8 value)
{
	switch (reg) {
	case 0:
		m_ArielAddress = value;
		m_ArielRgb = 0;
		break;
	case 1: {
		u8 *e = m_PaletteRam[m_ArielAddress];
		e[m_ArielRgb] = value;
		if (m_ArielRgb == 2) {
			m_Palette[m_ArielAddress] = ((u32)e[0] << 16) | ((u32)e[1] << 8) | e[2];
			++m_PaletteSerial;
		}
		if (++m_ArielRgb == 3) {
			m_ArielRgb = 0;
			++m_ArielAddress;
		}
		break;
	}
	case 2:
		m_ArielControl = value;
		break;
	case 3:
		m_ArielKey = value;
		break;
	default:
		break;
	}
}

// --- VIA1 wiring --------------------------------------------------------------

u8 MacV8::ViaInputA()
{
	// Model ID bits; bit 0 = 1, not in diagnostic mode (Eagle)
	if (m_Sonora)
		return 0x80; // as MAME (Sonora machines are told apart by the ID register)
	return m_Spice ? 0x82 : 0x93;
}

u8 MacV8::ViaInputB()
{
	return m_Egret.XcvrHigh() ? 0x08 : 0x00;
}

void MacV8::ViaOutputA(u8 ora, u8 ddra)
{
	const u8 pins = (u8)((ora & ddra) | ~ddra);
	m_Iwm.SetSel((pins & 0x20) != 0); // PA5: head select
}

void MacV8::ViaOutputB(u8 orb, u8 ddrb)
{
	const u8 pins = (u8)((orb & ddrb) | ~ddrb);
	if ((pins ^ m_PortBPins) & 0x30) {
		m_Egret.PortB(pins, Now());
		if (m_InSlice)
			cpu::EndTimeslice();
	}
	m_PortBPins = pins;
}

void MacV8::ViaIrq(bool asserted)
{
	m_ViaIrq = asserted;
	UpdateIpl();
}

void MacV8::ViaTimingChanged()
{
	if (m_InSlice)
		cpu::EndTimeslice();
}

void MacV8::UpdateIpl()
{
	// Only the highest source drives the IPL lines.
	unsigned ipl = 0;
	if (m_Scc.Irq())
		ipl = 4;
	else if (m_PvRegs[0x03] & 0x80)
		ipl = 2;
	else if (m_ViaIrq)
		ipl = 1;
	if (ipl != m_Ipl) {
		m_Ipl = ipl;
		cpu::SetIrq(ipl);
	}
}

// --- Frame loop ---------------------------------------------------------------

void MacV8::ServiceEvents()
{
	const u64 now = m_Cycles;
	m_Via.Update(now);
	m_Egret.Update(now);
	m_Scc.Update(now);

	// Sound: one FIFO sample per line, taken in chunks.
	const unsigned line = (unsigned)((now - m_FrameStart) / kCyclesPerLine);
	while (m_SoundLine < kLines && m_SoundLine <= line)
		m_Sound[m_SoundLine++] = m_Asc.NextSample();
	PseudoViaRecalc();

	if (now >= m_NextVbl) {
		m_PvRegs[0x02] &= (u8)~0x40; // VBL (slot interrupt 0x40)
		m_Via.SignalCA1();           // 60.15 Hz tick
		m_Egret.Frame(now);
		PseudoViaRecalc();
		m_NextVbl += kCyclesPerFrame;
	}
	UpdateIpl();
}

void MacV8::RunFrame()
{
	if (!m_Running)
		return;

	// Virtual memory (System 7's PRAM byte 0x0B bit 5, set by the Memory
	// control panel) needs page faults to restart the faulted instruction,
	// which needs a per-instruction register snapshot (~20% slower). On as
	// soon as VM is enabled; off again only once VM is disabled and the MMU
	// has been switched off since (VM stays active until a restart).
	if (m_Rtc.Pram()[0x0B] & 0x20) {
		mmu::TakeDisabled();
		cpu::SetRollback(true);
	} else if (mmu::TakeDisabled()) {
		cpu::SetRollback(false);
	}

	const u64 frameEnd = m_FrameStart + kCyclesPerFrame;
	while (m_Cycles < frameEnd) {
		u64 target = frameEnd;
		u64 e = m_Via.NextEvent();
		if (e < target) target = e;
		e = m_Egret.NextEvent();
		if (e < target) target = e;
		e = m_Scc.NextEvent();
		if (e < target) target = e;
		if (m_NextVbl < target) target = m_NextVbl;
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
	m_SoundLine = 0;
	m_PvRegs[0x02] |= 0x40; // end of VBL
	PseudoViaRecalc();
	m_FrameStart = frameEnd;
}

unsigned MacV8::SoundFrame(s16 *out, unsigned max, u32 &rateMilliHz)
{
	rateMilliHz = kSoundMilliHz;
	const unsigned n = max < (unsigned)kLines ? max : (unsigned)kLines;
	memcpy(out, m_Sound, n * sizeof(s16));
	return n;
}

MonoFrame MacV8::Screen() const
{
	MonoFrame f;
	if (m_Spice) { // only right at 1 bpp; hosts use ColorScreen()
		f.bits = m_Vram;
		f.width = kColorWidth;
		f.height = kColorHeight;
		f.rowBytes = m_Sonora ? kColorWidth / 8 : 1024;
		return f;
	}
	f.bits = m_MbRam + kScreenOffset;
	f.width = kWidth;
	f.height = kHeight;
	f.rowBytes = kRowBytes;
	return f;
}

bool MacV8::ColorScreen(ColorFrame &f) const
{
	if (!m_Spice)
		return false;
	static const unsigned kDepths[8] = {1, 2, 4, 8, 16, 1, 1, 1};
	f.bits = m_Vram;
	f.width = kColorWidth;
	f.height = kColorHeight;
	if (m_Sonora) {
		f.depth = kDepths[m_VDepth & 7];
		f.rowBytes = kColorWidth * f.depth / 8;
	} else {
		f.rowBytes = 1024;
		f.depth = kDepths[m_PvRegs[0x10] & 7];
	}
	f.palette = m_Palette;
	f.paletteSerial = m_PaletteSerial;
	return true;
}

// --- Input ------------------------------------------------------------------

void MacV8::KeyEvent(u8 macKey, bool down)
{
	const u8 code = AdbFromMacKey(macKey);
	if (code >= 0x80)
		return;
	if (code == kAdbCapsLock) {
		if (!down)
			return;
		m_CapsLocked = !m_CapsLocked;
		m_Adb.KeyEvent(code, m_CapsLocked);
		return;
	}
	const u8 bit = (u8)(1u << (code & 7));
	const bool wasDown = (m_KeysDown[code >> 3] & bit) != 0;
	if (down == wasDown)
		return;
	if (down)
		m_KeysDown[code >> 3] |= bit;
	else
		m_KeysDown[code >> 3] &= (u8)~bit;
	m_Adb.KeyEvent(code, down);
}

void MacV8::MouseMotion(int dx, int dy)
{
	m_Adb.MouseMotion(dx, dy);
}

void MacV8::MouseButton(bool down)
{
	m_Adb.MouseButton(down);
}

void MacV8::ReleaseAllInputs()
{
	for (unsigned code = 0; code < 128; ++code)
		if (m_KeysDown[code >> 3] & (1u << (code & 7)))
			m_Adb.KeyEvent((u8)code, false);
	memset(m_KeysDown, 0, sizeof m_KeysDown);
	m_Adb.MouseButton(false);
}

} // namespace pimac
