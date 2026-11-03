// Macintosh Classic II: a 68030 at 15.6672 MHz around Apple's "Eagle" ASIC
// (the V8 family of the Mac LC): RAM controller, VIA1, a pseudo-VIA
// (interrupt controller), the sound FIFO, and a 512x342 1-bit display
// fetched from main RAM. ADB, clock and PRAM live in an Egret
// microcontroller. SCC, 5380 SCSI and SWIM as on the Classic, at new
// addresses.
//
// 24-bit map (the ASIC decodes only A31 and A23-A0, so 32-bit addresses
// fold onto it):
//   000000-9FFFFF  RAM (the ROM while the reset overlay is on)
//   A00000-AFFFFF  ROM; any access ends the overlay
//   F00000 VIA1   F04000 SCC   F06000/F12000 SCSI pseudo-DMA
//   F10000 SCSI   F14000 sound F16000 SWIM   F26000 pseudo-VIA
// Interrupts: VIA1 level 1, pseudo-VIA level 2, SCC level 4.
// References: MAME's v8.cpp / maclc.cpp / pseudovia.cpp (hardware
// behaviour) and the Classic II ROM itself.
#pragma once

#include "mac/adb_devices.h"
#include "mac/asc.h"
#include "mac/cpu.h"
#include "mac/egret.h"
#include "mac/iwm.h"
#include "mac/mac_machine.h"
#include "mac/rtc.h"
#include "mac/scc.h"
#include "mac/scsi.h"
#include "mac/via.h"

namespace pimac {

class MacV8 : public MacMachine, private CpuBus, private ViaListener {
public:
	enum {
		kWidth = 512,
		kHeight = 342,
		kRowBytes = kWidth / 8,
		kCyclesPerLine = 704, // one dot per CPU cycle
		kLines = 370,
		kCyclesPerFrame = kCyclesPerLine * kLines, // 260480 -> 60.15 Hz
		kCpuHz = 15667200,
		kMaxSlice = 4000,
		kScreenOffset = 0x1F9A80, // in motherboard RAM
	};
	static const u32 kSoundMilliHz = (u32)((u64)kCpuHz * 1000 / kCyclesPerLine);

	MacV8();
	~MacV8();

	// ramKB: 2048 (on board), 4096, 6144 or 10240 (plus 2, 4 or 8 MB of SIMMs).
	bool Start(const MacConfig &config) override;
	const char *Error() const override { return m_Error; }

	// Machine
	void Reset() override;
	void RunFrame() override;
	MonoFrame Screen() const override;
	void KeyEvent(u8 macKey, bool down) override;
	void MouseMotion(int dx, int dy) override;
	void MouseButton(bool down) override;
	void ReleaseAllInputs() override;
	unsigned SoundFrame(s16 *out, unsigned max, u32 &rateMilliHz) override;

	u64 Cycles() const { return m_Cycles; }
	Rtc &GetRtc() override { return m_Rtc; } // PRAM + clock storage (kept by the Egret)
	const Scsi5380 &GetScsi() const override { return m_Scsi; }
	Iwm &GetIwm() override { return m_Iwm; }
	Scc &GetScc() override { return m_Scc; }
	const Egret &GetEgret() const { return m_Egret; }
	bool Overlay() const override { return m_Overlay; }
	u8 *Ram() override { return m_MbRam; } // motherboard RAM (for dumps)
	u32 RamSize() const override { return kMbRam; }
	u32 PhysRead16(u32 address) { return BusRead16(address); } // for dumps

private:
	enum : u32 { kMbRam = 2 * 1024 * 1024, kPageShift = 16, kPages = 0xA00000 >> 16 };

	// CpuBus
	u32 BusRead8(u32 address) override;
	u32 BusRead16(u32 address) override;
	void BusWrite8(u32 address, u32 value) override;
	void BusWrite16(u32 address, u32 value) override;

	// ViaListener
	u8 ViaInputA() override;
	u8 ViaInputB() override;
	void ViaOutputA(u8 ora, u8 ddra) override;
	void ViaOutputB(u8 orb, u8 ddrb) override;
	void ViaIrq(bool asserted) override;
	void ViaTimingChanged() override;

	u64 Now() const;
	u8 *RamAt(u32 a) const; // 0 if no RAM there
	void MapRam(u8 config);
	void EndOverlay();
	u8 IoRead(u32 a);
	void IoWrite(u32 a, u8 value);
	u32 ScsiDmaRead(unsigned bytes);
	void ScsiDmaWrite(u32 value, unsigned bytes);
	u8 PseudoViaRead(unsigned reg);
	void PseudoViaWrite(unsigned reg, u8 value);
	void PseudoViaRecalc();
	void UpdateIpl();
	void ServiceEvents();

	u8 *m_MbRam;
	u8 *m_SimmRam;
	u32 m_SimmSize;
	u8 *m_Page[kPages]; // RAM by 64 KB page (after the overlay)
	const u8 *m_Rom;
	u32 m_RomMask;
	bool m_Overlay;
	bool m_Running;
	char m_Error[64];

	Via6522 m_Via;
	Rtc m_Rtc;
	AdbDevices m_Adb;
	Egret m_Egret;
	Scsi5380 m_Scsi;
	Iwm m_Iwm;
	Scc m_Scc;
	AscV8 m_Asc;

	// Pseudo-VIA (MAME numbering): [0x02] slot interrupts (active low,
	// 0x40 = VBL), [0x03] IFR, [0x12] slot enables, [0x13] IER.
	u8 m_PvRegs[0x14];
	u8 m_RamConfig; // pseudo-VIA register 1

	bool m_ViaIrq;
	u8 m_PortBPins;
	unsigned m_Ipl;

	u64 m_Cycles;
	bool m_InSlice;
	u64 m_FrameStart;
	u64 m_NextVbl;
	unsigned m_SoundLine; // next line to sample this frame
	s16 m_Sound[kLines];

	bool m_CapsLocked;
	u8 m_KeysDown[16];
};

} // namespace pimac
