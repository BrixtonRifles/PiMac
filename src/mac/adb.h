// VIA-based ADB transceiver (Mac SE / Classic) with an emulated ADB keyboard
// (address 2) and mouse (address 3).
//
// The Mac drives the transceiver state on VIA PB4/PB5 (ST0/ST1) and moves
// bytes through the VIA shift register; PB3 is the transceiver's /INT line.
// States: 0 = new command, 1 = even data byte, 2 = odd data byte, 3 = idle.
// Behaviour follows the byte-level model used successfully by Mini vMac.
#pragma once

#include "core/compat.h"
#include "mac/via.h"

namespace pimac {

class AdbTransceiver {
public:
	static const u64 kNever = ~(u64)0;
	static const u32 kResponseCycles = 5000; // transceiver latency after a state change

	AdbTransceiver();

	void Reset();
	void StateLinesChanged(unsigned state, u64 now);
	u64 NextEvent() const { return m_PendingAt; }
	void Update(u64 now, Via6522 &via); // act on a pending state change
	void Poll(Via6522 &via);            // ~60 Hz: auto-poll while idle
	bool InterruptLineHigh() const { return m_IntHigh; }

	// Host input. Key codes are ADB key codes; bit 7 is never set here.
	void KeyEvent(u8 adbCode, bool down);
	void MouseMotion(int dx, int dy);
	void MouseButton(bool down);

private:
	void DoNewState(Via6522 &via);
	void DoCommand();
	void Talk();
	void EndListen();
	bool AnyEvent() const;
	bool PopKey(u8 &event);
	void TalkMouse(unsigned reg);
	void TalkKeyboard(unsigned reg);
	void TalkRegister3();

	unsigned m_State;
	unsigned m_PendingState;
	u64 m_PendingAt;
	bool m_IntHigh;

	u8 m_Cmd;
	bool m_Listen;
	bool m_TalkData;
	u8 m_Buf[8];
	unsigned m_Size;
	unsigned m_Index;

	u8 m_KeyboardAddress;
	u8 m_MouseAddress;
	u8 m_RandomAddress;

	enum { kKeyQueue = 64 };
	u8 m_Keys[kKeyQueue];
	unsigned m_KeyHead, m_KeyTail;
	int m_Dx, m_Dy;
	bool m_Button, m_ButtonReported;
};

} // namespace pimac
