#include "mac/scc.h"

namespace pimac {

namespace {

const u64 kCpuHz = 7833600;   // compact Macs
const u64 kSccClock = 3672000; // PCLK and RTxC on the compact Macs
const u64 kNever = ~(u64)0;

// RR0
enum : u8 {
	Rr0RxAvailable = 0x01,
	Rr0TxEmpty = 0x04,
	Rr0Dcd = 0x08,
	Rr0SyncHunt = 0x10,
	Rr0Cts = 0x20,
	Rr0TxUnderrun = 0x40,
	Rr0Break = 0x80,
};
// RR1 (per received byte)
enum : u8 {
	Rr1AllSent = 0x01,
	Rr1Residue8 = 0x06, // SDLC: residue code 011 = 8 bits in the last byte
	Rr1Parity = 0x10,
	Rr1Overrun = 0x20,
	Rr1CrcFraming = 0x40,
	Rr1EndOfFrame = 0x80,
};

} // namespace

Scc::Scc() : m_Wr2(0), m_Wr9(0), m_Irq(false)
{
	for (unsigned i = 0; i < 2; ++i) {
		m_Ch[i].port = 0;
		m_Ch[i].lastDtr = false;
	}
	Reset();
}

void Scc::Reset()
{
	for (unsigned i = 0; i < 2; ++i) {
		Channel &c = m_Ch[i];
		memset(c.wr, 0, sizeof c.wr);
		c.wr[4] = 0x04; // 1 stop bit
		ResetChannel(c);
		c.rxNext = 0;
		c.lastLines = 0;
	}
	m_Wr9 = 0;
	m_Irq = false;
}

void Scc::ResetChannel(Channel &c)
{
	c.wr[1] = 0;
	c.wr[3] &= (u8)~0x01;
	c.wr[5] &= (u8)~0x9E;
	c.wr[15] = 0xF8;
	c.fifoCount = 0;
	c.rxFirstArmed = false;
	c.eof = false;
	c.frameLen = c.framePos = 0;
	c.hunt = true;
	c.txBusy = false;
	c.txDone = 0;
	c.txIP = false;
	c.txLen = 0;
	c.eom = true;
	c.extIP = false;
	c.extLatched = 0;
	c.pointer = 0;
}

void Scc::Attach(unsigned channel, SccPort *port)
{
	m_Ch[channel & 1].port = port;
}

bool Scc::Sdlc(const Channel &c) const
{
	// WR4: stop bits 00 = synchronous modes; sync mode 10 = SDLC.
	return (c.wr[4] & 0x0C) == 0 && (c.wr[4] & 0x30) == 0x20;
}

u64 Scc::CharCycles(const Channel &c) const
{
	static const unsigned kClockMode[4] = {1, 16, 32, 64};
	const u64 mode = kClockMode[c.wr[4] >> 6];
	u64 baud;
	if (c.wr[14] & 0x01) { // baud rate generator
		const u64 tc = (u64)c.wr[12] | ((u64)c.wr[13] << 8);
		baud = kSccClock / (2 * (tc + 2) * mode);
	} else {
		baud = kSccClock / mode; // clocked straight from RTxC (e.g. MIDI)
	}
	if (baud < 50)
		baud = 50;
	return kCpuHz * 10 / baud; // start + 8 data + stop
}

// --- Receive -------------------------------------------------------------

void Scc::Push(Channel &c, u8 value, u8 status)
{
	if (c.fifoCount >= 3) {
		c.fifoStatus[2] |= Rr1Overrun;
		c.fifo[2] = value;
		return;
	}
	c.fifo[c.fifoCount] = value;
	c.fifoStatus[c.fifoCount] = status;
	++c.fifoCount;
}

void Scc::SdlcFeed(Channel &c)
{
	// One byte at a time, as the ROM reads them: End of Frame shows (on the
	// second CRC byte) once the first CRC byte has been read.
	while (c.frameLen && c.fifoCount == 0) {
		const unsigned i = c.framePos++;
		u8 status = Rr1Residue8;
		if (i + 1 == c.frameLen) {
			status |= Rr1EndOfFrame;
			c.eof = true;
		}
		Push(c, c.frame[i], status);
		if (c.framePos >= c.frameLen)
			c.frameLen = c.framePos = 0; // all delivered
	}
}

void Scc::SdlcPoll(Channel &c)
{
	if (!c.port || !Sdlc(c) || !RxEnabled(c) || c.frameLen || c.fifoCount)
		return;
	for (;;) {
		const unsigned n = c.port->ReceiveFrame(c.frame, kMaxFrame);
		if (n == 0)
			return;
		// Address search: only frames for this node or broadcasts.
		if ((c.wr[3] & 0x04) && c.frame[0] != c.wr[6] && c.frame[0] != 0xFF)
			continue;
		c.frame[n] = c.frame[n + 1] = 0; // CRC (always good)
		c.frameLen = n + 2;
		c.framePos = 0;
		c.eof = false;
		c.hunt = false;
		SdlcFeed(c);
		return;
	}
}

u8 Scc::ReadData(Channel &c)
{
	if (!c.fifoCount)
		return 0;
	const u8 v = c.fifo[0];
	for (unsigned i = 1; i < c.fifoCount; ++i) {
		c.fifo[i - 1] = c.fifo[i];
		c.fifoStatus[i - 1] = c.fifoStatus[i];
	}
	--c.fifoCount;
	c.rxFirstArmed = false;
	if (Sdlc(c))
		SdlcFeed(c);
	return v;
}

// --- Transmit ------------------------------------------------------------

void Scc::WriteData(Channel &c, u8 value, u64 now)
{
	if (!TxEnabled(c))
		return;
	if (Sdlc(c)) {
		if (c.txLen < kMaxFrame)
			c.txFrame[c.txLen++] = value;
		c.txIP = true; // ready for the next byte at once
		return;
	}
	if (c.port)
		c.port->TxByte(value);
	c.txBusy = true;
	c.txDone = now + CharCycles(c);
	c.txIP = false;
}

// --- Status --------------------------------------------------------------

u8 Scc::LineBits(Channel &c)
{
	u8 v = 0;
	if (c.port && c.port->Dcd())
		v |= Rr0Dcd;
	if (c.port && c.port->Cts())
		v |= Rr0Cts;
	if (Sdlc(c) && c.hunt)
		v |= Rr0SyncHunt;
	if (c.eom)
		v |= Rr0TxUnderrun;
	return v;
}

void Scc::CheckLines(Channel &c)
{
	const u8 live = LineBits(c);
	const u8 changed = live ^ c.lastLines;
	c.lastLines = live;
	// WR15 enables each source; WR1 bit 0 enables the interrupt (and the
	// latching of RR0).
	const u8 enabled = c.wr[15] & (Rr0Dcd | Rr0SyncHunt | Rr0Cts | Rr0TxUnderrun | Rr0Break);
	if ((changed & enabled) && (c.wr[1] & 0x01) && !c.extIP) {
		c.extIP = true;
		c.extLatched = live;
	}
	const bool dtr = (c.wr[5] & 0x80) != 0;
	if (dtr != c.lastDtr) {
		c.lastDtr = dtr;
		if (c.port)
			c.port->Dtr(dtr);
	}
}

u8 Scc::ReadRR0(Channel &c)
{
	SdlcPoll(c);
	CheckLines(c);
	u8 v = (c.fifoCount ? Rr0RxAvailable : 0) | (c.txBusy ? 0 : Rr0TxEmpty);
	const u8 lines = c.extIP ? c.extLatched : LineBits(c);
	return v | (lines & (Rr0Dcd | Rr0SyncHunt | Rr0Cts | Rr0TxUnderrun | Rr0Break));
}

u8 Scc::ReadRR1(const Channel &c) const
{
	u8 v = c.txBusy ? 0 : Rr1AllSent;
	if (c.fifoCount)
		v |= c.fifoStatus[0];
	else
		v |= Rr1Residue8;
	if (c.eof)
		v |= Rr1EndOfFrame;
	return v;
}

// --- Interrupts ----------------------------------------------------------

bool Scc::SpecialPending(const Channel &c) const
{
	if (((c.wr[1] >> 3) & 3) == 0)
		return false;
	u8 special = Rr1Overrun | Rr1CrcFraming | Rr1EndOfFrame;
	if (c.wr[1] & 0x04)
		special |= Rr1Parity; // parity is a special condition
	return c.eof || (c.fifoCount && (c.fifoStatus[0] & special));
}

bool Scc::RxPending(const Channel &c) const
{
	if (!c.fifoCount)
		return false;
	switch ((c.wr[1] >> 3) & 3) {
	case 1:
		return c.rxFirstArmed;
	case 2:
		return true;
	default:
		return false;
	}
}

Scc::Source Scc::Highest() const
{
	static const unsigned kOrder[2] = {kChannelA, kChannelB};
	for (unsigned i = 0; i < 2; ++i) {
		const Channel &c = m_Ch[kOrder[i]];
		const unsigned base = i * 4;
		if (SpecialPending(c))
			return (Source)(base + 1);
		if (RxPending(c))
			return (Source)(base + 0);
		if (c.txIP && (c.wr[1] & 0x02))
			return (Source)(base + 2);
		if (c.extIP && (c.wr[1] & 0x01))
			return (Source)(base + 3);
	}
	return SrcNone;
}

void Scc::UpdateIrq()
{
	m_Irq = (m_Wr9 & 0x08) && Highest() != SrcNone;
}

u8 Scc::ReadRR2(bool channelB) const
{
	if (!channelB)
		return m_Wr2;
	// Channel B: the vector modified by the highest pending interrupt.
	static const u8 kCode[SrcNone + 1] = {
		6, 7, 4, 5, // A: Rx, special, Tx, ext
		2, 3, 0, 1, // B
		3,          // nothing pending
	};
	const u8 code = kCode[Highest()];
	if (m_Wr9 & 0x10) // status high: V4 V5 V6
		return (u8)((m_Wr2 & 0x8F) | ((code & 4) << 2) | ((code & 2) << 4) | ((code & 1) << 6));
	return (u8)((m_Wr2 & 0xF1) | (code << 1));
}

u8 Scc::ReadRR3() const
{
	const Channel &a = m_Ch[kChannelA], &b = m_Ch[kChannelB];
	u8 v = 0;
	if (b.extIP && (b.wr[1] & 0x01)) v |= 0x01;
	if (b.txIP && (b.wr[1] & 0x02)) v |= 0x02;
	if (RxPending(b) || SpecialPending(b)) v |= 0x04;
	if (a.extIP && (a.wr[1] & 0x01)) v |= 0x08;
	if (a.txIP && (a.wr[1] & 0x02)) v |= 0x10;
	if (RxPending(a) || SpecialPending(a)) v |= 0x20;
	return v;
}

// --- Register access -----------------------------------------------------

void Scc::Command(Channel &c, u8 value)
{
	switch ((value >> 3) & 7) {
	case 2: // reset external/status interrupts
		c.extIP = false;
		c.lastLines = LineBits(c);
		break;
	case 3: // send abort (SDLC)
		c.txLen = 0;
		break;
	case 4: // enable interrupt on next Rx character
		c.rxFirstArmed = true;
		break;
	case 5: // reset Tx interrupt pending
		c.txIP = false;
		break;
	case 6: // error reset
		c.eof = false;
		if (c.fifoCount)
			c.fifoStatus[0] &= (u8)~(Rr1Parity | Rr1Overrun | Rr1CrcFraming | Rr1EndOfFrame);
		break;
	default: // null, point high, reset highest IUS (no IACK on the Mac)
		break;
	}
	switch (value >> 6) {
	case 2: // reset Tx CRC generator: a new SDLC frame starts
		c.txLen = 0;
		break;
	case 3: // reset Tx underrun/EOM latch: the ROM sends this after the
		// last byte, so the frame is complete
		if (Sdlc(c) && c.txLen) {
			if (c.port)
				c.port->SendFrame(c.txFrame, c.txLen);
			c.txLen = 0;
		}
		c.eom = true; // the CRC and closing flag have gone out
		break;
	default:
		break;
	}
}

void Scc::WriteRegister(Channel &c, unsigned reg, u8 value, u64 now)
{
	switch (reg) {
	case 0:
		Command(c, value);
		break;
	case 1: {
		const unsigned oldMode = (c.wr[1] >> 3) & 3;
		c.wr[1] = value;
		if (((value >> 3) & 3) == 1 && oldMode != 1)
			c.rxFirstArmed = true;
		break;
	}
	case 2:
		m_Wr2 = value;
		break;
	case 3: {
		const bool wasEnabled = RxEnabled(c);
		c.wr[3] = value;
		if (value & 0x10)
			c.hunt = true; // enter hunt mode
		if (wasEnabled && !RxEnabled(c)) {
			c.fifoCount = 0;
			c.frameLen = c.framePos = 0; // abandon a frame in progress
			c.eof = false;
			c.hunt = true;
		} else if (!wasEnabled && RxEnabled(c)) {
			c.rxNext = now;
			SdlcPoll(c);
		}
		break;
	}
	case 5: {
		const bool wasEnabled = TxEnabled(c);
		c.wr[5] = value;
		if (!wasEnabled && TxEnabled(c))
			c.txLen = 0;
		break;
	}
	case 6:
		c.wr[6] = value;
		if (c.port)
			c.port->SetNodeAddress(value);
		break;
	case 8:
		WriteData(c, value, now);
		break;
	case 9:
		switch (value >> 6) {
		case 1:
			ResetChannel(m_Ch[kChannelB]);
			break;
		case 2:
			ResetChannel(m_Ch[kChannelA]);
			break;
		case 3:
			Reset();
			break;
		default:
			break;
		}
		m_Wr9 = value & 0x3F;
		break;
	default:
		c.wr[reg] = value;
		break;
	}
}

u8 Scc::Read(unsigned channel, bool data, u64 now)
{
	Update(now);
	Channel &c = m_Ch[channel & 1];
	u8 v = 0;
	if (data) {
		v = ReadData(c);
	} else {
		const unsigned reg = c.pointer;
		c.pointer = 0;
		switch (reg) {
		case 0:
		case 4:
			v = ReadRR0(c);
			break;
		case 1:
		case 5:
			v = ReadRR1(c);
			break;
		case 2:
		case 6:
			v = ReadRR2((channel & 1) == kChannelB);
			break;
		case 3:
		case 7:
			v = (channel & 1) == kChannelA ? ReadRR3() : 0;
			break;
		case 8:
			v = ReadData(c);
			break;
		case 12:
			v = c.wr[12];
			break;
		case 9:
		case 13:
			v = c.wr[13];
			break;
		case 11:
		case 15:
			v = c.wr[15] & 0xFA;
			break;
		default: // RR10, RR14: no loop/clock status
			v = 0;
			break;
		}
	}
	UpdateIrq();
	return v;
}

void Scc::Write(unsigned channel, bool data, u8 value, u64 now)
{
	Update(now);
	Channel &c = m_Ch[channel & 1];
	if (data) {
		WriteData(c, value, now);
	} else {
		const unsigned reg = c.pointer;
		c.pointer = 0;
		if (reg == 0) {
			c.pointer = (value & 7) | (((value >> 3) & 7) == 1 ? 8 : 0);
			Command(c, value);
		} else {
			WriteRegister(c, reg, value, now);
		}
	}
	CheckLines(c);
	UpdateIrq();
}

// --- Timed work ----------------------------------------------------------

void Scc::Update(u64 now)
{
	for (unsigned i = 0; i < 2; ++i) {
		Channel &c = m_Ch[i];
		if (c.txBusy && now >= c.txDone) {
			c.txBusy = false;
			c.txIP = true;
		}
		if (Sdlc(c)) {
			SdlcPoll(c);
		} else if (c.port && RxEnabled(c) && now >= c.rxNext) {
			u8 b;
			if (c.fifoCount < 3 && c.port->RxByte(&b))
				Push(c, b, 0);
			c.rxNext = now + CharCycles(c); // next byte, or look again
		}
		CheckLines(c);
	}
	UpdateIrq();
}

u64 Scc::NextEvent() const
{
	u64 next = kNever;
	for (unsigned i = 0; i < 2; ++i) {
		const Channel &c = m_Ch[i];
		if (c.txBusy && c.txDone < next)
			next = c.txDone;
		if (c.port && RxEnabled(c) && !Sdlc(c) && c.rxNext < next)
			next = c.rxNext;
	}
	return next;
}

} // namespace pimac
