// Egret: the Classic II's (and LC's, IIsi's) ADB / clock / PRAM / power
// microcontroller, emulated at the level of its packets rather than by
// running its 68HC05 firmware.
//
// The Mac talks to it through VIA1: the shift register (bytes, clocked by
// the Egret), PB5 = SYS_SESSION (Mac starts a transaction), PB4 = VIA_FULL
// (Mac's byte handshake), PB3 = XCVR_SESSION (Egret, active low: it is
// sending; released with the last byte).
//
//   Mac -> Egret: PB5 up; per byte: SR out, PB4 up, (Egret takes it: SR
//                 interrupt), PB4 down; PB5 down at the end.
//   Egret -> Mac: PB3 down + an attention byte; then per byte, after the
//                 Mac raises PB5 or ends its PB4 ack: the next byte (PB3 up
//                 with the last); the Mac lowers PB5 when done.
// Packets: type (0 ADB, 1 pseudo/Egret command, 2 error), then for replies
// a flag byte (ADB: 0x02 no reply, 0x40 autopoll), the command, data.
// References: the Classic II ROM's Egret code (~0x40A14C4C), NetBSD's
// adb_direct.c (IIsi), Linux via-cuda.c (Cuda, its successor).
//
// Cuda (Color Classic, LC 475...) speaks the same packets with a different
// handshake: PB5 = TIP (Mac, active low), PB4 = TACK (Mac, toggled per byte),
// PB3 = TREQ (Cuda, active low, same pin and polarity as XCVR).
//   Mac -> Cuda: SR out + first byte, TIP down (Cuda takes it); per further
//                byte: SR out, TACK toggled; TIP and TACK up at the end.
//   Cuda -> Mac: TREQ down + an attention byte; the Mac lowers TIP, Cuda
//                sends the first byte, then one per TACK toggle (TREQ up
//                before the last); after the Mac raises TIP, one more
//                "done" shift.
//   Sync (at startup, TIP negated): TACK down -> TREQ down + a shift;
//                TACK up -> TREQ up, then a shift (the ROM reads SR in
//                between and waits for a new one).
#pragma once

#include "core/compat.h"
#include "mac/adb_devices.h"
#include "mac/rtc.h"
#include "mac/via.h"

namespace pimac {

class Egret {
public:
	static const u64 kNever = ~(u64)0;

	// cyclesPerMs: CPU cycles per millisecond (for byte timing).
	Egret(Via6522 &via, AdbDevices &adb, Rtc &rtc, u32 cyclesPerMs);

	void Reset();
	void SetCuda(bool cuda) { m_Cuda = cuda; Reset(); }

	// VIA port B as the Mac drives it (bits 4 and 5 matter).
	void PortB(u8 pins, u64 now);
	bool XcvrHigh() const { return !m_Xcvr; } // PB3 level

	void Update(u64 now);
	u64 NextEvent() const { return m_EventAt; }

	// Every video frame: autopoll ADB, one-second ticks.
	void Frame(u64 now);

	// Counters for diagnostics / tests
	unsigned Commands() const { return m_Commands; }
	bool PowerOffRequested() const { return m_PowerOff; }

private:
	enum State { Idle, Receiving, Sending };
	enum Event { EvNone, EvTakeByte, EvRespond, EvSendByte, EvDone, EvSyncAssert, EvSyncNegate, EvSyncShift };

	void Schedule(Event e, u64 now);
	void Execute();               // the command in m_Cmd
	void PseudoCommand();
	void AdbCommand();
	void Reply(u8 type, u8 flag, u8 cmd, const u8 *data, unsigned len);
	void CudaPortB(u8 pins, u64 now);
	void StartSending(u64 now);
	void SendNext();
	bool StartUnsolicited(u64 now); // autopoll / tick, if any

	Via6522 &m_Via;
	AdbDevices &m_Adb;
	Rtc &m_Rtc;
	u32 m_ByteCycles;

	bool m_Cuda;
	bool m_Synced; // Cuda: sync handshake answered (TREQ asserted)
	State m_State;
	bool m_SysSession, m_ViaFull; // PB5, PB4 as last seen (Cuda: TIP asserted, TACK level)
	bool m_Xcvr;                  // asserting XCVR_SESSION (PB3 low)
	Event m_Event;
	u64 m_EventAt;

	u8 m_Cmd[40];
	unsigned m_CmdLen;
	u8 m_Out[4 + 256]; // attention byte + packet (PRAM reads run long)
	unsigned m_OutLen, m_OutPos;
	bool m_OutReady; // a reply is waiting to be sent

	bool m_AutoPoll;
	u8 m_OneSecondMode;
	u8 m_LastPolled; // ADB address autopoll tries first
	unsigned m_FramesToSecond;
	bool m_TickPending;
	unsigned m_Commands;
	bool m_PowerOff;
};

} // namespace pimac
