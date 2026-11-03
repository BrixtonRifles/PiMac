#include "mac/modem.h"

namespace pimac {

namespace {

const u32 kGuardMs = 1000;    // +++ guard time (S12 = 50, i.e. 1 s)
const u32 kRingMs = 6000;     // one ring cycle
const unsigned kMaxRings = 10; // then the caller is turned away
const u32 kDialTimeoutMs = 30000;

enum : u8 { kIac = 255, kDont = 254, kDo = 253, kWont = 252, kWill = 251, kSb = 250, kSe = 240 };
enum : u8 { kOptEcho = 1, kOptSga = 3 };

char Upper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

bool Digit(char c)
{
	return c >= '0' && c <= '9';
}

// Reads a decimal number at s[*i] (0 if there is none).
unsigned Number(const char *s, unsigned *i)
{
	unsigned v = 0;
	while (Digit(s[*i]))
		v = v * 10 + (unsigned)(s[(*i)++] - '0');
	return v;
}

// Writes v in decimal; returns the number of characters.
unsigned Decimal(unsigned v, char *out)
{
	char tmp[12];
	unsigned n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	for (unsigned i = 0; i < n; ++i)
		out[i] = tmp[n - 1 - i];
	out[n] = 0;
	return n;
}

} // namespace

HayesModem::HayesModem() : m_Link(0)
{
	m_Dtr = false;
	m_Now = 0;
	m_Remote[0] = 0;
	m_LastLine[0] = 0;
	Reset();
}

void HayesModem::Reset()
{
	if (m_Link)
		m_Link->Close();
	m_Mode = Command;
	m_OnlineCommand = false;
	m_Echo = true;
	m_Quiet = false;
	m_Verbose = true;
	memset(m_S, 0, sizeof m_S);
	m_S[2] = '+';
	m_S[3] = 13;
	m_S[4] = 10;
	m_S[5] = 8;
	m_S[7] = 30;
	m_Telnet = false;
	m_TelnetState = 0;
	m_TelnetCmd = 0;
	m_LineLen = 0;
	m_SawA = false;
	m_RxHead = m_RxCount = 0;
	m_TxCount = 0;
	m_LastTx = m_Now;
	m_Plus = 0;
	m_PlusTime = 0;
	m_Ringing = false;
	m_Rings = 0;
	m_NextRing = 0;
}

// --- To the Mac --------------------------------------------------------------

void HayesModem::PutRx(u8 c)
{
	if (m_RxCount == kRxSize)
		return;
	m_Rx[(m_RxHead + m_RxCount++) % kRxSize] = c;
}

bool HayesModem::RxByte(u8 *value)
{
	if (!m_RxCount)
		return false;
	*value = m_Rx[m_RxHead];
	m_RxHead = (m_RxHead + 1) % kRxSize;
	--m_RxCount;
	return true;
}

void HayesModem::Say(const char *text)
{
	while (*text)
		PutRx((u8)*text++);
}

void HayesModem::Respond(Result r)
{
	if (m_Quiet)
		return;
	if (!m_Verbose) {
		char num[12];
		Decimal((unsigned)r, num);
		Say(num);
		PutRx(m_S[3]);
		return;
	}
	static const char *const kText[] = {"OK", "CONNECT", "RING", "NO CARRIER", "ERROR",
					    "", "", "BUSY"};
	PutRx(m_S[3]);
	PutRx(m_S[4]);
	Say(kText[r]);
	PutRx(m_S[3]);
	PutRx(m_S[4]);
}

// --- From the Mac ------------------------------------------------------------

void HayesModem::TxByte(u8 value)
{
	const u32 quiet = m_Now - m_LastTx;
	m_LastTx = m_Now;

	if (m_Mode == Dialing) { // any key aborts the call
		HangUp(true);
		return;
	}
	if (m_Mode == Command || m_OnlineCommand) {
		CommandChar((char)value);
		return;
	}

	// Online. Watch for the escape: guard time, +++, guard time.
	if (value == m_S[2] && (m_Plus ? quiet < kGuardMs : quiet >= kGuardMs)) {
		if (++m_Plus == 3)
			m_PlusTime = m_Now;
	} else {
		m_Plus = 0;
	}
	if (m_TxCount + 2 > kTxSize)
		return; // the Mac ignored CTS
	if (m_Telnet && value == kIac)
		m_Tx[m_TxCount++] = kIac;
	m_Tx[m_TxCount++] = value;
}

void HayesModem::CommandChar(u8 c)
{
	if (m_Echo)
		PutRx(c);
	if (c == m_S[5] || c == 127) {
		if (m_LineLen)
			--m_LineLen;
		return;
	}
	// "A/" repeats the last command line at once.
	if (m_LineLen == 0) {
		if (Upper((char)c) == 'A') {
			m_SawA = true;
		} else if (m_SawA && c == '/') {
			m_SawA = false;
			memcpy(m_Line, m_LastLine, kLineSize);
			m_LineLen = (unsigned)strlen(m_Line);
			Execute();
			m_LineLen = 0;
			return;
		} else {
			m_SawA = false;
		}
	}
	if (c == m_S[3]) {
		m_Line[m_LineLen] = 0;
		m_SawA = false;
		if (m_LineLen >= 2 && Upper(m_Line[0]) == 'A' && Upper(m_Line[1]) == 'T') {
			memcpy(m_LastLine, m_Line, kLineSize);
			Execute();
		}
		m_LineLen = 0;
		return;
	}
	if (c == m_S[4] || c < 32)
		return;
	if (m_LineLen + 1 < kLineSize)
		m_Line[m_LineLen++] = (char)c;
}

void HayesModem::Execute()
{
	const char *s = m_Line;
	unsigned i = 2; // after "AT"
	for (;;) {
		const char c = Upper(s[i]);
		if (!c)
			break;
		++i;
		if (c == ' ')
			continue;
		switch (c) {
		case 'A':
			if (m_Ringing && m_Link && m_Link->Incoming()) {
				m_Link->Accept();
				m_Ringing = false;
				memcpy(m_Remote, "incoming", 9);
				m_Telnet = false;
				GoOnline();
			} else {
				Respond(NoCarrier);
			}
			return;
		case 'D': {
			while (s[i] == ' ' || Upper(s[i]) == 'T' || Upper(s[i]) == 'P')
				++i;
			Dial(s + i);
			return;
		}
		case 'E': m_Echo = Number(s, &i) != 0; break;
		case 'Q': m_Quiet = Number(s, &i) != 0; break;
		case 'V': m_Verbose = Number(s, &i) != 0; break;
		case 'H':
			Number(s, &i);
			if (m_Mode != Command) {
				m_Link->Close();
				m_Mode = Command;
				m_OnlineCommand = false;
				m_Telnet = false;
				m_TxCount = 0;
			}
			break;
		case 'I':
			Number(s, &i);
			PutRx(m_S[3]);
			PutRx(m_S[4]);
			Say("PiMac network modem");
			PutRx(m_S[3]);
			PutRx(m_S[4]);
			break;
		case 'O':
			Number(s, &i);
			if (m_Mode == Connected && m_OnlineCommand) {
				m_OnlineCommand = false;
				Respond(ConnectResult);
				m_LastTx = m_Now;
				return;
			}
			Respond(Error);
			return;
		case 'Z':
			Number(s, &i);
			Reset();
			break;
		case 'S': {
			const unsigned reg = Number(s, &i);
			if (s[i] == '=') {
				++i;
				const unsigned v = Number(s, &i);
				if (reg < sizeof m_S)
					m_S[reg] = (u8)v;
			} else if (s[i] == '?') {
				++i;
				char num[12];
				Decimal(reg < sizeof m_S ? m_S[reg] : 0, num);
				PutRx(m_S[3]);
				PutRx(m_S[4]);
				Say(num);
				PutRx(m_S[3]);
				PutRx(m_S[4]);
			} else {
				Respond(Error);
				return;
			}
			break;
		}
		case '&':
			if (Upper(s[i]) == 'F') {
				++i;
				Reset();
				break;
			}
			if (s[i])
				++i;
			Number(s, &i);
			break;
		case '\\':
		case '%':
			if (s[i])
				++i;
			Number(s, &i);
			break;
		default:
			if (c >= 'A' && c <= 'Z') { // B C L M N W X Y ...: accepted, ignored
				Number(s, &i);
				break;
			}
			Respond(Error);
			return;
		}
	}
	Respond(Ok);
}

// --- Connections -------------------------------------------------------------

void HayesModem::Dial(const char *target)
{
	// host[:port], or host port
	char host[64];
	unsigned n = 0, port = 23;
	while (*target && *target != ':' && *target != ' ' && *target != ';' && n + 1 < sizeof host)
		host[n++] = *target++;
	host[n] = 0;
	while (*target == ':' || *target == ' ')
		++target;
	if (Digit(*target)) {
		unsigned i = 0;
		port = Number(target, &i);
	}
	if (!n || !port || port > 65535 || !m_Link) {
		Respond(n ? NoCarrier : Error);
		return;
	}
	memcpy(m_Remote, host, n + 1);
	m_Telnet = port == 23;
	m_TelnetState = 0;
	m_Mode = Dialing;
	m_OnlineCommand = false;
	m_PlusTime = m_Now; // dial start, for the timeout
	m_Link->Connect(host, (u16)port);
}

void HayesModem::GoOnline()
{
	m_Mode = Connected;
	m_OnlineCommand = false;
	m_TxCount = 0;
	m_Plus = 0;
	m_LastTx = m_Now;
	Respond(ConnectResult);
}

void HayesModem::HangUp(bool sayNoCarrier)
{
	if (m_Link)
		m_Link->Close();
	m_Mode = Command;
	m_OnlineCommand = false;
	m_Telnet = false;
	m_TxCount = 0;
	m_Plus = 0;
	if (sayNoCarrier)
		Respond(NoCarrier);
}

void HayesModem::Dtr(bool on)
{
	if (m_Dtr && !on && m_Mode != Command)
		HangUp(true);
	m_Dtr = on;
}

void HayesModem::NetworkIn(const u8 *data, unsigned n)
{
	for (unsigned k = 0; k < n; ++k) {
		const u8 b = data[k];
		if (!m_Telnet) {
			PutRx(b);
			continue;
		}
		switch (m_TelnetState) {
		case 0:
			if (b == kIac)
				m_TelnetState = 1;
			else
				PutRx(b);
			break;
		case 1:
			m_TelnetState = 0;
			if (b == kIac)
				PutRx(b);
			else if (b == kDo || b == kDont || b == kWill || b == kWont) {
				m_TelnetCmd = b;
				m_TelnetState = 2;
			} else if (b == kSb)
				m_TelnetState = 3;
			break;
		case 2: {
			m_TelnetState = 0;
			u8 reply = 0;
			if (m_TelnetCmd == kDo)
				reply = kWont; // we do nothing special
			else if (m_TelnetCmd == kWill)
				reply = (b == kOptEcho || b == kOptSga) ? kDo : kDont;
			if (reply && m_TxCount + 3 <= kTxSize) {
				m_Tx[m_TxCount++] = kIac;
				m_Tx[m_TxCount++] = reply;
				m_Tx[m_TxCount++] = b;
			}
			break;
		}
		case 3: // subnegotiation: skip to IAC SE
			if (b == kIac)
				m_TelnetState = 4;
			break;
		case 4:
			m_TelnetState = b == kSe ? 0 : 3;
			break;
		}
	}
}

void HayesModem::Flush()
{
	if (!m_TxCount)
		return;
	const unsigned sent = m_Link->Send(m_Tx, m_TxCount);
	if (sent) {
		m_TxCount -= sent;
		for (unsigned i = 0; i < m_TxCount; ++i)
			m_Tx[i] = m_Tx[sent + i];
	}
}

void HayesModem::Poll(u32 nowMs)
{
	m_Now = nowMs;
	if (!m_Link)
		return;

	switch (m_Mode) {
	case Command:
		if (m_Link->Incoming()) {
			if (!m_Ringing) {
				m_Ringing = true;
				m_Rings = 0;
				m_NextRing = m_Now;
			}
			if ((s32)(m_Now - m_NextRing) >= 0) {
				if (m_Rings == kMaxRings) {
					m_Link->Close(); // nobody answered
					m_Ringing = false;
					break;
				}
				Respond(Ring);
				++m_Rings;
				m_NextRing = m_Now + kRingMs;
				if (m_S[0] && m_Rings >= m_S[0]) {
					m_Link->Accept();
					m_Ringing = false;
					memcpy(m_Remote, "incoming", 9);
					m_Telnet = false;
					GoOnline();
				}
			}
		} else {
			m_Ringing = false;
		}
		break;

	case Dialing: {
		const StreamLink::State st = m_Link->GetState();
		if (st == StreamLink::Connected)
			GoOnline();
		else if (st != StreamLink::Connecting ||
			 m_Now - m_PlusTime > (m_S[7] ? (u32)m_S[7] * 1000 : kDialTimeoutMs))
			HangUp(true);
		break;
	}

	case Connected:
		if (m_Link->GetState() != StreamLink::Connected) {
			HangUp(true);
			break;
		}
		Flush();
		if (!m_OnlineCommand) {
			if (m_Plus == 3 && m_Now - m_PlusTime >= kGuardMs && m_Now - m_LastTx >= kGuardMs) {
				m_Plus = 0;
				m_OnlineCommand = true;
				Respond(Ok);
				break;
			}
			u8 buf[256];
			while (m_RxCount + sizeof buf <= kRxSize) {
				const unsigned n = m_Link->Receive(buf, sizeof buf);
				if (!n)
					break;
				NetworkIn(buf, n);
			}
		}
		break;
	}
}

} // namespace pimac
