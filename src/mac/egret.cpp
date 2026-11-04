#include "mac/egret.h"

#include "core/log.h"

namespace pimac {

namespace {

enum : u8 { kAdbPacket = 0, kPseudoPacket = 1, kErrorPacket = 2, kTickPacket = 3 };

enum : u8 {
	kCmdAutoPoll = 0x01,
	kCmdReadMemory = 0x02,
	kCmdReadTime = 0x03,
	kCmdReadPram = 0x07,
	kCmdWriteMemory = 0x08,
	kCmdWriteTime = 0x09,
	kCmdPowerDown = 0x0A,
	kCmdWritePram = 0x0C,
	kCmdWriteDfac = 0x0E,
	kCmdResetCpu = 0x11,
	kCmdSetAutoRate = 0x14,
	kCmdGetAutoRate = 0x16,
	kCmdSetDevList = 0x19,
	kCmdGetDevList = 0x1A,
	kCmdOneSecondMode = 0x1B,
};

const unsigned kByteMicros = 200; // Egret byte time (handshake to next byte)

} // namespace

Egret::Egret(Via6522 &via, AdbDevices &adb, Rtc &rtc, u32 cyclesPerMs)
	: m_Via(via), m_Adb(adb), m_Rtc(rtc), m_ByteCycles(cyclesPerMs * kByteMicros / 1000),
	  m_Cuda(false)
{
	Reset();
}

void Egret::Reset()
{
	m_State = Idle;
	m_SysSession = false;
	m_ViaFull = m_Cuda; // Cuda: TACK idles high (negated)
	m_Xcvr = false;
	m_Synced = false;
	m_Event = EvNone;
	m_EventAt = kNever;
	m_CmdLen = 0;
	m_OutLen = m_OutPos = 0;
	m_OutReady = false;
	m_AutoPoll = false;
	m_OneSecondMode = 0;
	m_LastPolled = 3;
	m_FramesToSecond = 60;
	m_TickPending = false;
	m_Commands = 0;
	m_PowerOff = false;
}

void Egret::Schedule(Event e, u64 now)
{
	m_Event = e;
	m_EventAt = now + m_ByteCycles;
}

void Egret::PortB(u8 pins, u64 now)
{
	if (m_Cuda) {
		CudaPortB(pins, now);
		return;
	}
	const bool sys = (pins & 0x20) != 0, full = (pins & 0x10) != 0;
	const bool sysRose = sys && !m_SysSession, sysFell = !sys && m_SysSession;
	const bool fullRose = full && !m_ViaFull, fullFell = !full && m_ViaFull;
	m_SysSession = sys;
	m_ViaFull = full;

	switch (m_State) {
	case Idle:
		if (sysRose) {
			m_State = Receiving;
			m_CmdLen = 0;
			if (m_Event == EvRespond) // an unsolicited packet waits
				m_Event = EvNone, m_EventAt = kNever;
		}
		break;
	case Receiving:
		if (fullRose)
			Schedule(EvTakeByte, now); // shift the Mac's byte in
		if (sysFell) {
			Execute();
			m_State = Idle;
			if (m_OutReady)
				Schedule(EvRespond, now);
		}
		break;
	case Sending:
		if (sysFell && m_OutPos >= m_OutLen) {
			m_State = Idle; // done
			m_Xcvr = false;
			break;
		}
		if (sysFell) {
			// The Mac gave up part way (the boot code reads only the
			// first bytes of a reply).
			m_State = Idle;
			m_Xcvr = false;
			m_Event = EvNone;
			m_EventAt = kNever;
			break;
		}
		if ((sysRose || fullFell) && m_SysSession)
			Schedule(EvSendByte, now); // previous byte taken
		break;
	}
}

void Egret::CudaPortB(u8 pins, u64 now)
{
	const bool tip = !(pins & 0x20), tack = (pins & 0x10) != 0;
	const bool tipFell = tip && !m_SysSession, tipRose = !tip && m_SysSession;
	const bool tackToggled = tack != m_ViaFull;
	m_SysSession = tip;
	m_ViaFull = tack;

	switch (m_State) {
	case Idle:
		if (!tip && tackToggled) { // sync handshake
			Schedule(tack ? EvSyncNegate : EvSyncAssert, now);
			break;
		}
		if (tipFell) {
			m_State = Receiving;
			m_CmdLen = 0;
			if (m_Event == EvRespond || m_Event == EvDone)
				m_Event = EvNone, m_EventAt = kNever;
			Schedule(EvTakeByte, now); // the first byte is already in SR
		}
		break;
	case Receiving:
		if (tipRose) {
			if (m_Event == EvTakeByte) // TACK and TIP raised together
				m_Event = EvNone, m_EventAt = kNever;
			Execute();
			m_State = Idle;
			if (m_OutReady)
				Schedule(EvRespond, now);
		} else if (tackToggled && tip) {
			Schedule(EvTakeByte, now);
		}
		break;
	case Sending:
		if (tipRose) { // done (or the Mac read only what it wanted)
			m_State = Idle;
			m_Xcvr = false;
			Schedule(EvDone, now);
			break;
		}
		if (tipFell && m_Via.ShiftingOut()) {
			// Collision: the Mac starts a command without reading the
			// packet Cuda is sending (the ROM's polled routines after a
			// restart do). Cuda drops its packet and takes the command.
			m_State = Receiving;
			m_CmdLen = 0;
			m_Xcvr = false;
			m_Event = EvNone;
			m_EventAt = kNever;
			Schedule(EvTakeByte, now); // the first byte is already in SR
			break;
		}
		if (tipFell || (tackToggled && tip))
			Schedule(EvSendByte, now);
		break;
	}
}

void Egret::Update(u64 now)
{
	if (m_Event == EvNone || now < m_EventAt)
		return;
	const Event e = m_Event;
	m_Event = EvNone;
	m_EventAt = kNever;
	switch (e) {
	case EvTakeByte: {
		const u8 b = m_Via.DeviceShiftOut();
		if (m_State == Receiving && m_CmdLen < sizeof m_Cmd)
			m_Cmd[m_CmdLen++] = b;
		break;
	}
	case EvRespond:
		if (m_State == Idle && !m_SysSession)
			StartSending(now);
		else if (m_OutReady)
			Schedule(EvRespond, now); // retry after the Mac's transaction
		break;
	case EvSendByte:
		if (m_State == Sending)
			SendNext();
		break;
	case EvSyncAssert:
		if (m_State == Idle) {
			m_Synced = true;
			m_Xcvr = true;
			m_Via.DeviceShiftIn(0x00);
		}
		break;
	case EvSyncNegate:
		if (m_State == Idle && m_Synced) {
			m_Synced = false;
			m_Xcvr = false;
			Schedule(EvSyncShift, now);
		}
		break;
	case EvSyncShift:
		if (m_State == Idle)
			m_Via.DeviceShiftIn(0x00);
		break;
	case EvDone: // Cuda: the transaction-complete shift
		if (m_State == Idle)
			m_Via.DeviceShiftIn(0x00);
		break;
	default:
		break;
	}
}

void Egret::StartSending(u64 now)
{
	(void)now;
	m_OutReady = false;
	m_State = Sending;
	m_OutPos = 1;
	m_Xcvr = true;
	m_Via.DeviceShiftIn(m_Out[0]); // attention byte
}

void Egret::SendNext()
{
	if (m_OutPos >= m_OutLen)
		return;
	if (m_OutPos == m_OutLen - 1)
		m_Xcvr = false; // last byte
	m_Via.DeviceShiftIn(m_Out[m_OutPos++]);
}

void Egret::Reply(u8 type, u8 flag, u8 cmd, const u8 *data, unsigned len)
{
	if (len > sizeof m_Out - 4)
		len = sizeof m_Out - 4;
	m_Out[0] = 0x00; // attention
	m_Out[1] = type;
	m_Out[2] = flag;
	m_Out[3] = cmd;
	if (len)
		memcpy(m_Out + 4, data, len);
	m_OutLen = 4 + len;
	m_OutReady = true;
}

void Egret::Execute()
{
	++m_Commands;
	if (m_CmdLen < 2) {
		if (m_CmdLen == 1)
			Reply(kErrorPacket, 0, m_Cmd[0], 0, 0);
		return;
	}
	switch (m_Cmd[0]) {
	case kAdbPacket:
		AdbCommand();
		break;
	case kPseudoPacket:
		PseudoCommand();
		break;
	default: {
		const u8 data[1] = {m_Cmd[1]};
		Reply(kErrorPacket, 0, m_Cmd[0], data, 1);
		break;
	}
	}
}

void Egret::AdbCommand()
{
	const u8 cmd = m_Cmd[1];
	u8 reply[8];
	const int n = m_Adb.Command(cmd, m_Cmd + 2, m_CmdLen - 2, reply);
	if (n < 0)
		Reply(kAdbPacket, 0x02, cmd, 0, 0); // no reply (timeout)
	else
		Reply(kAdbPacket, 0x00, cmd, reply, (unsigned)n);
	if (((cmd >> 2) & 3) == 3)
		m_LastPolled = cmd >> 4;
}

void Egret::PseudoCommand()
{
	const u8 cmd = m_Cmd[1];
	const u8 *arg = m_Cmd + 2;
	const unsigned argLen = m_CmdLen - 2;
	u8 data[256];
	unsigned len = 0;

	switch (cmd) {
	case kCmdAutoPoll:
		m_AutoPoll = argLen && arg[0];
		break;
	case kCmdReadTime: {
		const u32 t = m_Rtc.Seconds();
		data[0] = (u8)(t >> 24);
		data[1] = (u8)(t >> 16);
		data[2] = (u8)(t >> 8);
		data[3] = (u8)t;
		len = 4;
		break;
	}
	case kCmdWriteTime:
		if (argLen >= 4)
			m_Rtc.SetSeconds(((u32)arg[0] << 24) | ((u32)arg[1] << 16) |
					 ((u32)arg[2] << 8) | arg[3]);
		break;
	case kCmdReadPram:
		// From the address to the end of PRAM: the Mac reads as many bytes
		// as it wants, then ends the session.
		if (argLen >= 2) {
			const unsigned addr = ((unsigned)arg[0] << 8 | arg[1]) & 0xFF;
			len = Rtc::kPramSize - addr;
			memcpy(data, m_Rtc.Pram() + addr, len);
		}
		break;
	case kCmdWritePram:
		if (argLen >= 2) {
			unsigned addr = ((unsigned)arg[0] << 8 | arg[1]) & 0xFF;
			for (unsigned i = 2; i < argLen; ++i) {
				m_Rtc.Pram()[addr] = arg[i];
				addr = (addr + 1) & 0xFF;
			}
			m_Rtc.MarkPramDirty();
		}
		break;
	case kCmdReadMemory:
		// Egret's own memory. PRAM lives in its RAM at 0x100-0x1FF (the
		// ROM checks and initializes PRAM that way); anything else reads 0.
		if (argLen >= 2) {
			const unsigned addr = (unsigned)arg[0] << 8 | arg[1];
			if (addr >= 0x100 && addr < 0x200) {
				len = 0x200 - addr;
				memcpy(data, m_Rtc.Pram() + (addr & 0xFF), len);
				break;
			}
		}
		data[0] = 0;
		len = 1;
		break;
	case kCmdWriteMemory:
		if (argLen >= 2) {
			unsigned addr = (unsigned)arg[0] << 8 | arg[1];
			for (unsigned i = 2; i < argLen; ++i, ++addr)
				if (addr >= 0x100 && addr < 0x200)
					m_Rtc.Pram()[addr & 0xFF] = arg[i];
			m_Rtc.MarkPramDirty();
		}
		break;
	case kCmdPowerDown:
		m_PowerOff = true;
		break;
	case kCmdOneSecondMode:
		m_OneSecondMode = argLen ? arg[0] : 0;
		break;
	case kCmdGetAutoRate:
		data[0] = 11; // ms
		len = 1;
		break;
	case kCmdGetDevList:
		data[0] = 0;
		data[1] = (u8)((1u << m_Adb.KeyboardAddress()) | (1u << m_Adb.MouseAddress()));
		len = 2;
		break;
	default:
		break; // accepted: WriteDFAC, SetAutoRate, SetDevList, ResetCPU, ...
	}
	Reply(kPseudoPacket, 0, cmd, data, len);
}

void Egret::Frame(u64 now)
{
	if (--m_FramesToSecond == 0) {
		m_FramesToSecond = 60;
		m_Rtc.Tick();
		if (m_OneSecondMode)
			m_TickPending = true;
	}
	if (m_State == Idle && m_Event == EvNone && !m_SysSession)
		StartUnsolicited(now);
}

bool Egret::StartUnsolicited(u64 now)
{
	if (m_TickPending) {
		m_TickPending = false;
		Reply(kTickPacket, 0, 0, 0, 0);
		m_OutLen = 2; // attention + type only
		StartSending(now);
		return true;
	}
	if (!m_AutoPoll)
		return false;
	// Poll the device that last had data first, then the other.
	const u8 first = m_LastPolled == m_Adb.KeyboardAddress() ? m_Adb.KeyboardAddress()
								   : m_Adb.MouseAddress();
	const u8 second = first == m_Adb.KeyboardAddress() ? m_Adb.MouseAddress()
							    : m_Adb.KeyboardAddress();
	const u8 address = m_Adb.HasData(first) ? first : m_Adb.HasData(second) ? second : 0xFF;
	if (address == 0xFF)
		return false;
	const u8 cmd = (u8)((address << 4) | 0x0C); // Talk register 0
	u8 reply[8];
	const int n = m_Adb.Command(cmd, 0, 0, reply);
	if (n <= 0)
		return false;
	m_LastPolled = address;
	Reply(kAdbPacket, 0x40, cmd, reply, (unsigned)n);
	StartSending(now);
	return true;
}

} // namespace pimac
