// Macintosh SE/30: a Mac IIx (68030 at 15.6672 MHz, GLUE, two VIAs, the
// original Apple Sound Chip, SWIM, 5380 SCSI, SCC) with a built-in 512x342
// 1-bit display in a compact case. ROM 97221136 (shared with the II FDHD,
// IIx and IIcx; VIA1 PA6 and VIA2 PB tell it which model it is on) plus the
// display's 8 KB declaration ROM (se30vrom.uk6), which makes the video look
// like a NuBus card in slot E. After MAME's macii.cpp (macse30).
//
// 32-bit map (the ROM uses the 68030 MMU for 24-bit mode):
//   00000000  RAM, two banks placed by VIA2 PA6-7 (the ROM's RAM sizing);
//             the ROM while the overlay (VIA1 PA4) is on
//   40000000  ROM (256 KB, mirrored)
//   50000000  I/O, mirrored every 1 MB: VIA1 +0 (and +40000), VIA2 +2000,
//             SCC +4000, SCSI +10000 (pseudo-DMA +6000, +12000),
//             ASC +14000, SWIM +16000
//   FE000000  64 KB video RAM (also FEE00000-FEEFFFFF): main screen at
//             +8040, alternate at +0040 (VIA1 PA6), 64 bytes a row
//   FEFFE000  the video declaration ROM
// FPU: the SE/30 always has a 68882 (the ROM sets HwCfgFlags' FPU bit
// unconditionally, and System 7 then uses the ROM's FPU SANE), emulated by
// src/fpu (cpu::SetFpu). The other 68030 Macs here have none.
// Interrupts: VIA1 level 1, VIA2 level 2 (CA1 = slot interrupts, slot E =
// VBL, enabled by VIA1 PB6 low; CB1 = ASC), SCC level 4. VIA2's timer 1
// output on PB7 makes the 60.15 Hz tick on VIA1 CA1. ADB (VIA shift
// register) and the clock/PRAM chip work as on the SE and Classic.
#pragma once

#include "mac/adb.h"
#include "mac/adb_input.h"
#include "mac/asc.h"
#include "mac/cpu.h"
#include "mac/iwm.h"
#include "mac/mac_machine.h"
#include "mac/rtc.h"
#include "mac/scc.h"
#include "mac/scsi.h"
#include "mac/via.h"

namespace pimac {

class MacSE30 : public MacMachine, private CpuBus, private ViaListener {
public:
	enum {
		kWidth = 512,
		kHeight = 342,
		kRowBytes = kWidth / 8,
		kCyclesPerLine = 704,
		kLines = 370,
		kCyclesPerFrame = kCyclesPerLine * kLines, // 260480 -> 60.15 Hz
		kCpuHz = 15667200,
		kMaxSlice = 4000,
		kVramSize = 0x10000,
		kVideoRomSize = 0x2000,
	};
	static const u32 kSoundMilliHz = (u32)((u64)kCpuHz * 1000 / kCyclesPerLine);

	MacSE30();
	~MacSE30();

	// The video declaration ROM, which is not part of the main ROM image
	// (8 KB; copied). Start fails without it.
	void SetVideoRom(const u8 *data, unsigned size);
	bool HasVideoRom() const { return m_HaveVideoRom; }

	// ramKB: 4096, 5120, 8192, 16384, 20480, 32768, 65536 or 131072.
	bool Start(const MacConfig &config) override;
	const char *Error() const override { return m_Error; }

	// Machine
	void Reset() override;
	void RunFrame() override;
	MonoFrame Screen() const override;
	void KeyEvent(u8 macKey, bool down) override { m_Input.KeyEvent(macKey, down); }
	void MouseMotion(int dx, int dy) override { m_Input.MouseMotion(dx, dy); }
	void MouseButton(bool down) override { m_Input.MouseButton(down); }
	void ReleaseAllInputs() override { m_Input.ReleaseAll(); }
	unsigned SoundFrame(s16 *out, unsigned max, u32 &rateMilliHz) override;

	u64 Cycles() const { return m_Cycles; }
	Rtc &GetRtc() override { return m_Rtc; }
	const Scsi5380 &GetScsi() const override { return m_Scsi; }
	Scsi5380 &GetScsi() override { return m_Scsi; }
	Iwm &GetIwm() override { return m_Iwm; }
	Scc &GetScc() override { return m_Scc; }
	bool Overlay() const override { return m_Overlay; }
	u8 *Ram() override { return m_Ram; }
	u32 RamSize() const override { return m_RamSize; }

private:
	enum : u32 { kPageShift = 16, kPages = 0x10000000 >> 16 }; // RAM and mirrors below 256 MB

	// VIA2's wiring (VIA1's is this class's ViaListener)
	class Via2Wiring : public ViaListener {
	public:
		explicit Via2Wiring(MacSE30 &mac) : m_Mac(mac) {}
		u8 ViaInputA() override;
		u8 ViaInputB() override;
		void ViaOutputA(u8 ora, u8 ddra) override;
		void ViaOutputB(u8 orb, u8 ddrb) override;
		void ViaIrq(bool asserted) override;
		void ViaTimingChanged() override;

	private:
		MacSE30 &m_Mac;
	};
	friend class Via2Wiring;

	// CpuBus
	u32 BusRead8(u32 address) override;
	u32 BusRead16(u32 address) override;
	void BusWrite8(u32 address, u32 value) override;
	void BusWrite16(u32 address, u32 value) override;

	// ViaListener (VIA1)
	u8 ViaInputA() override;
	u8 ViaInputB() override;
	void ViaOutputA(u8 ora, u8 ddra) override;
	void ViaOutputB(u8 orb, u8 ddrb) override;
	void ViaIrq(bool asserted) override;
	void ViaTimingChanged() override;

	u64 Now() const;
	u8 *RamAt(u32 a) const;
	void MapRam(u8 glue);
	void SetOverlay(bool on);
	void UpdateFastMap();
	u8 IoRead(u32 off);
	void IoWrite(u32 off, u8 value);
	u32 ScsiDmaRead(unsigned bytes);
	void ScsiDmaWrite(u32 value, unsigned bytes);
	void SetSlotE(bool asserted);
	void Tick60(); // VIA2 PB7 edge -> VIA1 CA1
	void CheckPb7();
	void CheckAscIrq();
	void UpdateIpl();
	void ServiceEvents();

	u8 *m_Ram;
	u32 m_RamSize;
	u8 *m_Page[kPages];
	u8 *m_Vram;
	u8 m_VideoRom[kVideoRomSize];
	bool m_HaveVideoRom;
	const u8 *m_Rom;
	u32 m_RomMask;
	bool m_Overlay;
	bool m_Running;
	char m_Error[64];

	Via2Wiring m_Via2Wiring;
	Via6522 m_Via1;
	Via6522 m_Via2;
	Rtc m_Rtc;
	AdbTransceiver m_Adb;
	Scsi5380 m_Scsi;
	Iwm m_Iwm;
	Scc m_Scc;
	AscClassic m_Asc;

	bool m_Via1Irq, m_Via2Irq;
	unsigned m_Ipl;
	unsigned m_AdbState;
	bool m_MainScreen;  // VIA1 PA6
	bool m_VblEnabled;  // VIA1 PB6 low
	u8 m_Glue;          // VIA2 PA6-7: RAM bank B position
	u8 m_SlotIrqs;      // VIA2 PA0-5, active low (bit 5 = slot E = video)
	bool m_Pb7Pin;      // VIA2 PB7 as last seen
	u32 m_Pb7Toggles;
	bool m_AscIrq;

	u64 m_Cycles;
	bool m_InSlice;
	u64 m_FrameStart;
	u64 m_NextVbl;
	u64 m_VblEnd;
	u64 m_NextSecond;
	unsigned m_SoundLine;
	s16 m_Sound[kLines];

	AdbInput<AdbTransceiver> m_Input; // host keys and mouse -> m_Adb
};

} // namespace pimac
