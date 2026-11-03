// A Hayes-compatible modem on the modem port that "dials" TCP: ATDT
// host[:port] opens a connection (port 23 by default, with minimal telnet
// handling), so Mac terminal programs can reach telnet BBSes. Incoming
// connections to a listening port ring (ATA answers, ATS0=n auto-answers).
//
// Commands: A D E H I O Q V X Z &F &D S0=n Sn? +++ (1 s guard time),
// all others answer OK. DCD is on while connected; dropping DTR hangs up.
#pragma once

#include "core/net.h"
#include "mac/scc.h"

namespace pimac {

class HayesModem : public SccPort {
public:
	HayesModem();

	void SetLink(StreamLink *link) { m_Link = link; } // not owned
	void Reset();

	// Call regularly (every video frame) with a millisecond clock: moves data
	// between the network and the buffers, and times rings and +++.
	void Poll(u32 nowMs);

	bool Online() const { return m_Mode != Command; }
	const char *Remote() const { return m_Remote; } // last host dialled or "incoming"

	// SccPort
	void TxByte(u8 value) override;
	bool RxByte(u8 *value) override;
	bool Dcd() override { return m_Mode == Connected; }
	bool Cts() override { return m_TxCount + 64 < kTxSize; } // room to send
	void Dtr(bool on) override;

private:
	enum Mode { Command, Dialing, Connected };
	enum Result { Ok = 0, ConnectResult = 1, Ring = 2, NoCarrier = 3, Error = 4, Busy = 7 };
	enum : unsigned { kRxSize = 4096, kTxSize = 1024, kLineSize = 80 };

	void CommandChar(u8 c);
	void Execute();
	void Respond(Result r);
	void Say(const char *text); // to the Mac, raw
	void PutRx(u8 c);
	void HangUp(bool sayNoCarrier);
	void Dial(const char *target);
	void GoOnline();
	void NetworkIn(const u8 *data, unsigned n);
	void Flush();

	StreamLink *m_Link;
	Mode m_Mode;
	bool m_OnlineCommand; // connected but in command mode after +++
	bool m_Echo, m_Quiet, m_Verbose;
	u8 m_S[16];
	bool m_Telnet;
	unsigned m_TelnetState;
	u8 m_TelnetCmd;

	char m_Line[kLineSize];
	unsigned m_LineLen;
	bool m_SawA; // "A" typed, waiting for "T" (or "/")
	char m_LastLine[kLineSize];

	u8 m_Rx[kRxSize];
	unsigned m_RxHead, m_RxCount;
	u8 m_Tx[kTxSize];
	unsigned m_TxCount;

	u32 m_Now;
	u32 m_LastTx;     // when the Mac last sent a byte
	unsigned m_Plus;  // '+' typed after a guard time
	u32 m_PlusTime;
	bool m_Ringing;
	unsigned m_Rings;
	u32 m_NextRing;
	bool m_Dtr;
	char m_Remote[64];
};

} // namespace pimac
