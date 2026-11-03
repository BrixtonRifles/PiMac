// Compact Macintosh with ADB: Macintosh SE and Macintosh Classic.
// 68000 @ 7.8336 MHz, 512x342 1-bit display, VIA, SCC, IWM, NCR 5380 SCSI,
// RTC with extended PRAM, VIA-based ADB transceiver.
#pragma once

#include "core/blockdev.h"
#include "core/machine.h"
#include "mac/adb.h"
#include "mac/cpu.h"
#include "mac/iwm.h"
#include "mac/rtc.h"
#include "mac/scc.h"
#include "mac/scsi.h"
#include "mac/via.h"

namespace pimac {

struct MacConfig {
	unsigned ramKB;   // 1024, 2048 or 4096
	const u8 *rom;    // caller keeps this alive
	unsigned romSize; // power of two
	BlockDevice *disk; // SCSI ID 0, may be 0
	u32 clockSeconds; // initial RTC value (seconds since 1904)
};

class MacCompact : public Machine, private CpuBus, private ViaListener {
public:
	enum {
		kWidth = 512,
		kHeight = 342,
		kRowBytes = kWidth / 8,
		kCyclesPerLine = 352, // 704 dots at 2 dots per CPU cycle
		kLines = 370,         // 342 visible + 28 blanking
		kCyclesPerFrame = kCyclesPerLine * kLines, // 130240 -> 60.15 Hz
		kCpuHz = 7833600,
		kMaxSlice = 2000,
	};

	MacCompact();
	~MacCompact();

	// Returns false if the configuration is unusable (message in Error()).
	bool Start(const MacConfig &config);
	const char *Error() const { return m_Error; }

	// Machine
	void Reset() override;
	void RunFrame() override;
	MonoFrame Screen() const override;
	void KeyEvent(u8 macKey, bool down) override;
	void MouseMotion(int dx, int dy) override;
	void MouseButton(bool down) override;
	void ReleaseAllInputs() override;
	unsigned SoundFrame(s16 *out, unsigned max, u32 &rateMilliHz) override;

	// One sample per scan line: 22254.5 Hz.
	static const u32 kSoundMilliHz = (u32)((u64)kCpuHz * 1000 / kCyclesPerLine);

	// Diagnostics / persistence
	u64 Cycles() const { return m_Cycles; }
	Rtc &GetRtc() { return m_Rtc; }
	const Scsi5380 &GetScsi() const { return m_Scsi; }
	Iwm &GetIwm() { return m_Iwm; } // floppy drive: Insert/Remove/TakeEjected
	Scc &GetScc() { return m_Scc; } // serial ports: Attach
	bool Overlay() const { return m_Overlay; }
	u8 *Ram() { return m_Ram; }
	u32 RamSize() const { return m_RamSize; }

private:
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
	u8 IoRead(u32 address);
	void IoWrite(u32 address, u8 value);
	void RomAccessed() { m_Overlay = false; }
	void UpdateIpl();
	void ServiceEvents();
	void CaptureSound(unsigned firstLine, unsigned endLine);

	u8 *m_Ram;
	u32 m_RamSize, m_RamMask;
	const u8 *m_Rom;
	u32 m_RomMask;
	bool m_Overlay;
	bool m_Running;
	char m_Error[64];

	Via6522 m_Via;
	Rtc m_Rtc;
	AdbTransceiver m_Adb;
	Scsi5380 m_Scsi;
	Iwm m_Iwm;
	Scc m_Scc;

	bool m_ViaIrq;
	bool m_MainScreen; // VIA PA6
	u8 m_PortA;        // VIA port A pins as last driven (volume, sound page)
	u8 m_PortADdr;
	bool m_SoundOn;    // VIA PB7 low
	s16 m_Sound[kLines]; // this frame's samples, one per scan line
	unsigned m_AdbState;

	u64 m_Cycles;     // cycles completed before the current slice
	bool m_InSlice;
	u64 m_FrameStart; // cycle at which the current video frame began
	u64 m_NextVbl;
	u64 m_NextSecond;

	bool m_CapsLocked;
	u8 m_KeysDown[16]; // bitmap of ADB codes currently down
};

} // namespace pimac
