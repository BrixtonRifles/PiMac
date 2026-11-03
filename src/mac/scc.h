// Zilog Z8530 SCC: the Mac's two serial ports. Channel A is the modem port,
// channel B the printer port (where LocalTalk runs).
//
// Emulated at the level the Mac's drivers see:
//   - async mode: bytes are paced at the programmed baud rate (receive) and
//     the transmit buffer stays full for one character time, with Rx, Tx and
//     external/status interrupts;
//   - SDLC mode (LocalTalk): whole frames. The bytes the Mac writes between
//     "Reset Tx CRC" and "Reset Tx Underrun/EOM" are one frame (that is how
//     the ROM's LocalTalk driver sends); received frames are handed over byte
//     by byte with two CRC bytes and End of Frame, and address search filters
//     frames for other nodes.
// What is attached to each port is an SccPort (nothing, by default).
//
// References: Zilog SCC/ESCC User's Manual; Mini vMac's SCCEMDEV.c for how
// the ROM's LocalTalk driver uses the chip; the Classic ROM's LAP code
// (~0x4337AC-0x433A40).
#pragma once

#include "core/compat.h"

namespace pimac {

// Whatever is plugged into one serial port.
class SccPort {
public:
	virtual ~SccPort() {}

	// Async mode
	virtual void TxByte(u8 value) { (void)value; }
	virtual bool RxByte(u8 *value) // next incoming byte, if any
	{
		(void)value;
		return false;
	}
	virtual bool Dcd() { return false; } // GPi (carrier detect)
	virtual bool Cts() { return false; } // HSKi (handshake in)
	virtual void Dtr(bool on) { (void)on; } // HSKo (handshake out)

	// SDLC mode: one frame without its CRC.
	virtual void SendFrame(const u8 *frame, unsigned len)
	{
		(void)frame;
		(void)len;
	}
	// Copies the next incoming frame into `frame`; 0 if none.
	virtual unsigned ReceiveFrame(u8 *frame, unsigned max)
	{
		(void)frame;
		(void)max;
		return 0;
	}
	// The node address the Mac set for address search (WR6).
	virtual void SetNodeAddress(u8 node) { (void)node; }
};

class Scc {
public:
	enum : unsigned { kChannelB = 0, kChannelA = 1 }; // as addressed (A1)
	enum : unsigned { kMaxFrame = 640 };

	Scc();

	void Reset(); // hardware reset; ports stay attached
	void Attach(unsigned channel, SccPort *port); // 0 = nothing

	// channel: kChannelB/kChannelA; data: the data register (A2)
	u8 Read(unsigned channel, bool data, u64 now);
	void Write(unsigned channel, bool data, u8 value, u64 now);

	// Timed work (receive pacing, transmit completion, port lines).
	void Update(u64 now);
	u64 NextEvent() const; // CPU cycle of the next timed event

	bool Irq() const { return m_Irq; }

private:
	struct Channel {
		SccPort *port;
		u8 wr[16];

		// Receive FIFO (3 deep), with the RR1 status of each byte.
		u8 fifo[3], fifoStatus[3];
		unsigned fifoCount;
		bool rxFirstArmed; // "interrupt on first/next Rx character"
		bool eof;          // SDLC End of Frame latched until Error Reset
		u64 rxNext;        // async: when the next byte may arrive

		// SDLC frame being received (+2 CRC bytes).
		u8 frame[kMaxFrame + 2];
		unsigned frameLen, framePos;
		bool hunt; // Sync/Hunt: no frame in progress

		// Transmit
		bool txBusy;   // buffer full (async: until txDone)
		u64 txDone;
		bool txIP;     // Tx buffer empty interrupt pending
		u8 txFrame[kMaxFrame];
		unsigned txLen;
		bool eom;      // Tx Underrun/EOM latch

		// External/status
		bool extIP;
		u8 extLatched; // RR0 as latched when extIP was set
		u8 lastLines;  // last RR0 line bits seen (DCD, CTS, Sync/Hunt)
		bool lastDtr;

		unsigned pointer; // register pointer (WR0)
	};

	void ResetChannel(Channel &c);
	bool Sdlc(const Channel &c) const;
	bool RxEnabled(const Channel &c) const { return (c.wr[3] & 0x01) != 0; }
	bool TxEnabled(const Channel &c) const { return (c.wr[5] & 0x08) != 0; }
	u64 CharCycles(const Channel &c) const;

	u8 LineBits(Channel &c); // live DCD/CTS/Sync-Hunt bits of RR0
	u8 ReadRR0(Channel &c);
	u8 ReadRR1(const Channel &c) const;
	u8 ReadRR2(bool channelB) const;
	u8 ReadRR3() const;
	u8 ReadData(Channel &c);
	void WriteData(Channel &c, u8 value, u64 now);
	void WriteRegister(Channel &c, unsigned reg, u8 value, u64 now);
	void Command(Channel &c, u8 value);

	void SdlcPoll(Channel &c); // start delivering the next frame
	void SdlcFeed(Channel &c); // frame bytes into the FIFO
	void CheckLines(Channel &c);
	void Push(Channel &c, u8 value, u8 status);

	// Interrupt sources by priority (highest first).
	enum Source {
		SrcARx, SrcASpecial, SrcATx, SrcAExt,
		SrcBRx, SrcBSpecial, SrcBTx, SrcBExt,
		SrcNone
	};
	Source Highest() const;
	bool RxPending(const Channel &c) const;
	bool SpecialPending(const Channel &c) const;
	void UpdateIrq();

	Channel m_Ch[2]; // indexed by kChannelB / kChannelA
	u8 m_Wr2;        // interrupt vector
	u8 m_Wr9;        // master interrupt control
	bool m_Irq;
};

} // namespace pimac
