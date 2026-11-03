#include "mac/mac_v8.h"

#include "core/log.h"

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

MacV8::MacV8()
	: m_MbRam(0), m_SimmRam(0), m_SimmSize(0), m_Rom(0), m_RomMask(0), m_Overlay(true),
	  m_Running(false), m_Via(this, 20), m_Egret(m_Via, m_Adb, m_Rtc, kCpuHz / 1000),
	  m_RamConfig(0), m_ViaIrq(false), m_PortBPins(0xFF), m_Ipl(0),
	  m_Cycles(0), m_InSlice(false), m_FrameStart(0), m_NextVbl(0), m_SoundLine(0),
	  m_CapsLocked(false)
{
	m_Error[0] = '\0';
	memset(m_Page, 0, sizeof m_Page);
	memset(m_PvRegs, 0, sizeof m_PvRegs);
	memset(m_Sound, 0, sizeof m_Sound);
	memset(m_KeysDown, 0, sizeof m_KeysDown);
}

MacV8::~MacV8()
{
	delete[] m_MbRam;
	delete[] m_SimmRam;
}

bool MacV8::Start(const MacConfig &config)
{
	m_Running = false;
	if (config.ramKB != 2048 && config.ramKB != 4096 && config.ramKB != 6144 &&
	    config.ramKB != 10240) {
		StrCopy(m_Error, "RAM must be 2, 4, 6 or 10 MB", sizeof m_Error);
		return false;
	}
	if (!config.rom || config.romSize < 0x80000 || (config.romSize & (config.romSize - 1))) {
		StrCopy(m_Error, "ROM image missing or bad size", sizeof m_Error);
		return false;
	}
	delete[] m_MbRam;
	delete[] m_SimmRam;
	m_MbRam = new u8[kMbRam];
	m_SimmSize = config.ramKB * 1024 - kMbRam; // 0, 2, 4 or 8 MB of SIMMs
	m_SimmRam = m_SimmSize ? new u8[m_SimmSize] : 0;
	m_Rom = config.rom;
	m_RomMask = config.romSize - 1;

	for (unsigned id = 0; id < Scsi5380::kTargets; ++id)
		m_Scsi.Attach(id, 0);
	if (config.disk)
		m_Scsi.Attach(0, config.disk);
	m_Rtc.SetSeconds(config.clockSeconds);

	cpu::Init(this);
	cpu::SetType(M68K_CPU_TYPE_68030);
	Reset();
	m_Running = true;
	return true;
}

void MacV8::Reset()
{
	memset(m_MbRam, 0, kMbRam);
	if (m_SimmRam)
		memset(m_SimmRam, 0, m_SimmSize);
	m_Overlay = true;
	memset(m_Page, 0, sizeof m_Page);
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
	m_NextVbl = m_FrameStart + (u64)kHeight * kCyclesPerLine;
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
		const u32 mbSize = (config & 0x20) ? 0x200000 : kMbRam;
		for (u32 a = 0; a < mbSize && mbAt + a < 0x800000; a += page)
			m_Page[(mbAt + a) >> kPageShift] = m_MbRam + a;
	}
}

void MacV8::EndOverlay()
{
	m_Overlay = false;
	MapRam(0xC0); // as MAME: "full SIMM" until the ROM sizes the RAM
}

inline u8 *MacV8::RamAt(u32 a) const
{
	u8 *p = m_Page[a >> kPageShift];
	return p ? p + (a & ((1u << kPageShift) - 1)) : 0;
}

// --- Bus --------------------------------------------------------------------

u32 MacV8::BusRead8(u32 a)
{
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
	return IoRead(a);
}

u32 MacV8::BusRead16(u32 a)
{
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
	if ((a >= 0xF06000 && a < 0xF08000) || (a >= 0xF12000 && a < 0xF14000))
		return ScsiDmaRead(2);
	const u8 v = IoRead(a);
	return ((u32)v << 8) | v;
}

void MacV8::BusWrite8(u32 a, u32 v)
{
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
	IoWrite(a, (u8)v);
}

void MacV8::BusWrite16(u32 a, u32 v)
{
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
		return PseudoViaRead(a & 0x13);
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
		PseudoViaWrite(a & 0x13, value);
		return;
	}
	if (a >= 0xF24000 && a < 0xF26000)
		return; // Ariel CLUT (not on the Classic II's mono display)
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
		return (u8)(m_RamConfig | 0x04);
	case 0x10:
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
		if (!m_Overlay)
			MapRam(value);
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

// --- VIA1 wiring --------------------------------------------------------------

u8 MacV8::ViaInputA()
{
	return 0x93; // Eagle: model ID bits; bit 0 = 1, not in diagnostic mode
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
	f.bits = m_MbRam + kScreenOffset;
	f.width = kWidth;
	f.height = kHeight;
	f.rowBytes = kRowBytes;
	return f;
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
