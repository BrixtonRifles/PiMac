#include "mac/mac_compact.h"
#include "core/log.h"

namespace pimac {

namespace {

// Mac virtual key code -> ADB key code (they differ only for these).
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

} // namespace

MacCompact::MacCompact()
	: m_Ram(0), m_RamSize(0), m_RamMask(0), m_Rom(0), m_RomMask(0),
	  m_Overlay(true), m_Running(false), m_Via(this),
	  m_ViaIrq(false), m_MainScreen(true), m_PortA(0), m_PortADdr(0), m_SoundOn(false),
	  m_AdbState(3),
	  m_Cycles(0), m_InSlice(false), m_FrameStart(0), m_NextVbl(0), m_NextSecond(0),
	  m_CapsLocked(false)
{
	m_Error[0] = '\0';
	memset(m_KeysDown, 0, sizeof m_KeysDown);
	memset(m_Sound, 0, sizeof m_Sound);
}

MacCompact::~MacCompact()
{
	delete[] m_Ram;
}

bool MacCompact::Start(const MacConfig &config)
{
	m_Running = false;
	if (config.ramKB != 1024 && config.ramKB != 2048 && config.ramKB != 4096) {
		StrCopy(m_Error, "RAM must be 1, 2 or 4 MB", sizeof m_Error);
		return false;
	}
	if (!config.rom || config.romSize < 0x20000 || (config.romSize & (config.romSize - 1))) {
		StrCopy(m_Error, "ROM image missing or bad size", sizeof m_Error);
		return false;
	}

	delete[] m_Ram;
	m_RamSize = config.ramKB * 1024;
	m_RamMask = m_RamSize - 1;
	m_Ram = new u8[m_RamSize];
	m_Rom = config.rom;
	m_RomMask = config.romSize - 1;

	for (unsigned id = 0; id < Scsi5380::kTargets; ++id)
		m_Scsi.Attach(id, 0);
	if (config.disk)
		m_Scsi.Attach(0, config.disk);
	m_Rtc.SetSeconds(config.clockSeconds);

	cpu::Init(this); // also resets, which reads the vectors - so map first
	Reset();
	m_Running = true;
	return true;
}

void MacCompact::Reset()
{
	memset(m_Ram, 0, m_RamSize);
	m_Overlay = true;
	m_MainScreen = true;
	m_PortA = m_PortADdr = 0;
	m_SoundOn = false;
	memset(m_Sound, 0, sizeof m_Sound);
	m_ViaIrq = false;
	m_AdbState = 3;
	m_Via.Reset();
	m_Rtc.Reset();
	m_Adb.Reset();
	m_Scsi.Reset();
	m_Iwm.Reset();
	m_Scc.Reset();

	m_FrameStart = m_Cycles;
	m_NextVbl = m_FrameStart + kHeight * kCyclesPerLine;
	m_NextSecond = m_Cycles + kCpuHz;

	UpdateIpl();
	cpu::Reset();
}

u64 MacCompact::Now() const
{
	return m_InSlice ? m_Cycles + (u64)cpu::CyclesRun() : m_Cycles;
}

// --- Memory map (24-bit) ---------------------------------------------------
//
//   000000-3FFFFF  RAM (ROM while the reset overlay is active)
//   400000-57FFFF  ROM (mirrored); any access ends the overlay (SE/Classic)
//   580000-5FFFFF  SCSI 5380 (reg = A4-A6, pseudo-DMA when A9 set)
//   600000-7FFFFF  RAM (overlay-era alias)
//   800000-9FFFFF  SCC read     A00000-BFFFFF  SCC write
//   C00000-DFFFFF  IWM          E80000-EFFFFF  VIA
//
// The SCC (read), VIA and 5380 (read) sit on the upper data byte (even
// addresses); the SCC (write), IWM and 5380 (write) on the lower byte.

u32 MacCompact::BusRead8(u32 a)
{
	a &= 0xFFFFFF;
	if (a < 0x400000)
		return m_Overlay ? m_Rom[a & m_RomMask] : m_Ram[a & m_RamMask];
	if (a < 0x580000) {
		RomAccessed();
		return m_Rom[a & m_RomMask];
	}
	return IoRead(a);
}

u32 MacCompact::BusRead16(u32 a)
{
	a &= 0xFFFFFF;
	if (a < 0x400000) {
		if (!m_Overlay) {
			const u8 *p = m_Ram;
			return ((u32)p[a & m_RamMask] << 8) | p[(a + 1) & m_RamMask];
		}
		return ((u32)m_Rom[a & m_RomMask] << 8) | m_Rom[(a + 1) & m_RomMask];
	}
	if (a < 0x580000) {
		RomAccessed();
		return ((u32)m_Rom[a & m_RomMask] << 8) | m_Rom[(a + 1) & m_RomMask];
	}
	u8 v = IoRead(a); // one bus cycle: one device access, both lanes
	return ((u32)v << 8) | v;
}

void MacCompact::BusWrite8(u32 a, u32 v)
{
	a &= 0xFFFFFF;
	if (a < 0x400000) {
		if (!m_Overlay)
			m_Ram[a & m_RamMask] = (u8)v;
		return;
	}
	if (a < 0x580000)
		return; // ROM
	IoWrite(a, (u8)v);
}

void MacCompact::BusWrite16(u32 a, u32 v)
{
	a &= 0xFFFFFF;
	if (a < 0x400000) {
		if (!m_Overlay) {
			m_Ram[a & m_RamMask] = (u8)(v >> 8);
			m_Ram[(a + 1) & m_RamMask] = (u8)v;
		}
		return;
	}
	if (a < 0x580000)
		return;
	if (a >= 0x600000 && a < 0x800000) {
		m_Ram[a & m_RamMask] = (u8)(v >> 8);
		m_Ram[(a + 1) & m_RamMask] = (u8)v;
		return;
	}
	// One bus cycle: hand the device the byte on the lane it is wired to.
	const bool lowLane = a >= 0xA00000 && a < 0xE00000; // SCC write, IWM
	IoWrite(a, lowLane ? (u8)v : (u8)(v >> 8));
}

u8 MacCompact::IoRead(u32 a)
{
	if (a < 0x600000)
		return m_Scsi.Read((a >> 4) & 7, (a & 0x200) != 0);
	if (a < 0x800000)
		return m_Ram[a & m_RamMask];
	if (a < 0xA00000) {
		const u8 v = m_Scc.Read((a >> 1) & 1, (a & 4) != 0, Now());
		UpdateIpl();
		return v;
	}
	if (a < 0xC00000) {
		LogHex("read from SCC write space", a, 0);
		return 0xFF;
	}
	if (a < 0xE00000)
		return m_Iwm.Access((a >> 9) & 0xF, false, 0, Now());
	if (a >= 0xE80000 && a < 0xF00000)
		return m_Via.Read((a >> 9) & 0xF, Now());
	LogHex("read unmapped", a, cpu::PC());
	return 0;
}

void MacCompact::IoWrite(u32 a, u8 value)
{
	if (a < 0x600000) {
		m_Scsi.Write((a >> 4) & 7, value, (a & 0x200) != 0);
		return;
	}
	if (a < 0x800000) {
		m_Ram[a & m_RamMask] = value;
		return;
	}
	if (a < 0xA00000) {
		LogHex("write to SCC read space", a, value);
		return;
	}
	if (a < 0xC00000) {
		m_Scc.Write((a >> 1) & 1, (a & 4) != 0, value, Now());
		UpdateIpl();
		return;
	}
	if (a < 0xE00000) {
		m_Iwm.Access((a >> 9) & 0xF, true, value, Now());
		return;
	}
	if (a >= 0xE80000 && a < 0xF00000) {
		m_Via.Write((a >> 9) & 0xF, value, Now());
		return;
	}
	LogHex("write unmapped", a, value);
}

// --- VIA wiring -----------------------------------------------------------

u8 MacCompact::ViaInputA()
{
	// PA7: SCC W/REQ, idle high. PA3 reads low when it is an input: that is
	// how the Classic ROM tells a Classic from an SE (it sets BoxFlag from
	// it). The SE ROM always drives PA3 as an output, so this is safe there.
	return (u8)~0x08;
}

u8 MacCompact::ViaInputB()
{
	u8 v = 0xFF & (u8)~(0x01 | 0x08 | 0x40);
	v |= m_Rtc.DataBit() ? 0x01 : 0;
	v |= m_Adb.InterruptLineHigh() ? 0x08 : 0;
	const u32 lineCycle = (u32)((Now() - m_FrameStart) % kCyclesPerLine);
	v |= lineCycle >= kWidth / 2 ? 0x40 : 0; // H4: horizontal blanking
	return v;
}

void MacCompact::ViaOutputA(u8 ora, u8 ddra)
{
	const u8 pins = (u8)((ora & ddra) | ~ddra);
	m_MainScreen = (pins & 0x40) != 0; // PA6: 1 = main screen buffer
	m_PortA = pins;
	m_PortADdr = ddra;
	m_Iwm.SetSel((pins & 0x20) != 0); // PA5: floppy SEL / head select
}

void MacCompact::ViaOutputB(u8 orb, u8 ddrb)
{
	const u8 pins = (u8)((orb & ddrb) | ~ddrb);
	m_SoundOn = (pins & 0x80) == 0; // PB7: /sound enable
	m_Rtc.PinsChanged(pins & 0x07, (ddrb & 0x01) != 0);

	const unsigned state = (pins >> 4) & 3; // PB4 = ST0, PB5 = ST1
	if (state != m_AdbState) {
		m_AdbState = state;
		m_Adb.StateLinesChanged(state, Now());
		if (m_InSlice)
			cpu::EndTimeslice();
	}
}

void MacCompact::ViaIrq(bool asserted)
{
	m_ViaIrq = asserted;
	UpdateIpl();
}

void MacCompact::ViaTimingChanged()
{
	if (m_InSlice)
		cpu::EndTimeslice();
}

void MacCompact::UpdateIpl()
{
	// The VIA drives IPL0 and the SCC IPL1: level 1, 2, or 3 for both.
	cpu::SetIrq((m_ViaIrq ? 1u : 0u) | (m_Scc.Irq() ? 2u : 0u));
}

// --- Frame loop -----------------------------------------------------------

void MacCompact::ServiceEvents()
{
	const u64 now = m_Cycles;
	m_Via.Update(now);
	m_Adb.Update(now, m_Via);
	m_Scc.Update(now);
	UpdateIpl();
	if (now >= m_NextVbl) {
		// Lines 0..341 played the buffer as it was before this VBL, when the
		// Sound Manager refills it.
		CaptureSound(0, kHeight);
		m_Via.SignalCA1();
		m_Adb.Poll(m_Via);
		m_NextVbl += kCyclesPerFrame;
	}
	if (now >= m_NextSecond) {
		m_Rtc.Tick();
		m_Via.SignalCA2();
		m_NextSecond += kCpuHz;
	}
}

void MacCompact::RunFrame()
{
	if (!m_Running)
		return;

	const u64 frameEnd = m_FrameStart + kCyclesPerFrame;
	while (m_Cycles < frameEnd) {
		u64 target = frameEnd;
		u64 e = m_Via.NextEvent();
		if (e < target) target = e;
		e = m_Adb.NextEvent();
		if (e < target) target = e;
		e = m_Scc.NextEvent();
		if (e < target) target = e;
		if (m_NextVbl < target) target = m_NextVbl;
		if (m_NextSecond < target) target = m_NextSecond;
		if (m_Cycles + kMaxSlice < target) target = m_Cycles + kMaxSlice;

		int n = target > m_Cycles ? (int)(target - m_Cycles) : 1;
		m_InSlice = true;
		int ran = cpu::Execute(n);
		m_InSlice = false;
		m_Cycles += ran > 0 ? (u64)ran : 1;

		ServiceEvents();
	}
	CaptureSound(kHeight, kLines);
	m_FrameStart = frameEnd;
}

void MacCompact::CaptureSound(unsigned firstLine, unsigned endLine)
{
	// The sound buffer is 370 words at the top of RAM; the hardware plays the
	// high byte of one word per scan line. PA3 low selects the alternate
	// buffer on the SE (the Classic has none and leaves PA3 as an input).
	const bool alternate = (m_PortADdr & 0x08) && !(m_PortA & 0x08);
	const u32 base = m_RamSize - (alternate ? 0x5F00 : 0x300);
	if (!m_SoundOn) {
		for (unsigned line = firstLine; line < endLine; ++line)
			m_Sound[line] = 0;
		return;
	}
	const s32 volume = m_PortA & 7; // PA0..PA2
	for (unsigned line = firstLine; line < endLine; ++line) {
		const s32 sample = (s32)m_Ram[base + 2 * line] - 128;
		m_Sound[line] = (s16)(sample * 256 * volume / 7);
	}
}

unsigned MacCompact::SoundFrame(s16 *out, unsigned max, u32 &rateMilliHz)
{
	rateMilliHz = kSoundMilliHz;
	unsigned n = max < (unsigned)kLines ? max : (unsigned)kLines;
	memcpy(out, m_Sound, n * sizeof(s16));
	return n;
}

MonoFrame MacCompact::Screen() const
{
	MonoFrame f;
	f.bits = m_Ram + (m_RamSize - (m_MainScreen ? 0x5900 : 0xD900));
	f.width = kWidth;
	f.height = kHeight;
	f.rowBytes = kRowBytes;
	return f;
}

// --- Input ----------------------------------------------------------------

void MacCompact::KeyEvent(u8 macKey, bool down)
{
	u8 code = AdbFromMacKey(macKey);
	if (code >= 0x80)
		return;

	if (code == kAdbCapsLock) {
		// ADB Caps Lock is a locking key: it reports down while locked.
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

void MacCompact::MouseMotion(int dx, int dy)
{
	m_Adb.MouseMotion(dx, dy);
}

void MacCompact::MouseButton(bool down)
{
	m_Adb.MouseButton(down);
}

void MacCompact::ReleaseAllInputs()
{
	for (unsigned code = 0; code < 128; ++code)
		if (m_KeysDown[code >> 3] & (1u << (code & 7)))
			m_Adb.KeyEvent((u8)code, false);
	memset(m_KeysDown, 0, sizeof m_KeysDown);
	m_Adb.MouseButton(false);
}

} // namespace pimac
