#include "mac/scsi.h"
#include "core/log.h"

namespace pimac {

namespace {

const u8 kStatusGood = 0x00;
const u8 kStatusCheckCondition = 0x02;

const u8 kSenseNone = 0x00;
const u8 kSenseMediumError = 0x03;
const u8 kSenseIllegalRequest = 0x05;
const u8 kSenseDataProtect = 0x07;

const u8 kAscInvalidOpcode = 0x20;
const u8 kAscLbaOutOfRange = 0x21;
const u8 kAscWriteProtected = 0x27;

unsigned CommandLength(u8 opcode)
{
	switch (opcode >> 5) {
	case 0:
		return 6;
	case 1:
	case 2:
		return 10;
	case 5:
		return 12;
	default:
		return 6;
	}
}

void PutBE32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

void PutPadded(u8 *p, const char *s, unsigned n)
{
	for (unsigned i = 0; i < n; ++i)
		p[i] = *s ? (u8)*s++ : ' ';
}

} // namespace

Scsi5380::Scsi5380() : m_BlocksRead(0), m_BlocksWritten(0), m_IoError(false)
{
	for (unsigned i = 0; i < kTargets; ++i)
		m_Disks[i] = 0;
	Reset();
}

void Scsi5380::Attach(unsigned id, BlockDevice *disk)
{
	if (id < kTargets)
		m_Disks[id] = disk;
}

void Scsi5380::Reset()
{
	m_Odr = m_Icr = m_Mode = m_Tcr = 0;
	m_Irq = false;
	m_SenseKey = kSenseNone;
	m_Asc = 0;
	BusFree();
}

void Scsi5380::BusFree()
{
	m_Target = -1;
	m_Bsy = false;
	m_Req = false;
	m_Phase = PhNone;
	m_Data = 0;
	m_AckSeen = false;
	m_DiskTransfer = false;
	m_CmdPos = m_CmdLen = 0;
	m_BufLen = m_BufPos = 0;
}

void Scsi5380::EnterPhase(u8 phase)
{
	m_Phase = phase;
	m_AckSeen = false;
	if (phase == PhCommand) {
		m_CmdPos = 0;
		m_CmdLen = 0;
	}
	if (IsInput(phase))
		PresentInputByte();
	m_Req = true;
}

void Scsi5380::PresentInputByte()
{
	switch (m_Phase) {
	case PhDataIn:
		m_Data = m_BufPos < m_BufLen ? m_Buf[m_BufPos] : 0;
		break;
	case PhStatus:
		m_Data = m_Status;
		break;
	case PhMsgIn:
		m_Data = 0x00; // COMMAND COMPLETE
		break;
	default:
		break;
	}
}

bool Scsi5380::PhaseMatch() const
{
	return m_Bsy && m_Phase != PhNone && (m_Phase & 7) == (m_Tcr & 7);
}

u8 Scsi5380::BusStatus() const
{
	u8 v = 0;
	if (m_Icr & IcrRst)
		v |= 0x80;
	if (m_Bsy || (m_Icr & IcrBsy))
		v |= 0x40;
	if (m_Req)
		v |= 0x20;
	if (m_Bsy && m_Phase != PhNone) {
		if (m_Phase & 4)
			v |= 0x10; // MSG
		if (m_Phase & 2)
			v |= 0x08; // C/D
		if (m_Phase & 1)
			v |= 0x04; // I/O
	}
	if (m_Icr & IcrSel)
		v |= 0x02;
	return v;
}

u8 Scsi5380::BusAndStatus() const
{
	u8 v = 0;
	if (PhaseMatch())
		v |= 0x08;
	if ((m_Mode & ModeDma) && m_Req && PhaseMatch())
		v |= 0x40; // DRQ
	if (m_Irq)
		v |= 0x10;
	if (m_Icr & IcrAtn)
		v |= 0x02;
	if (m_Icr & IcrAck)
		v |= 0x01;
	return v;
}

u8 Scsi5380::Read(unsigned reg, bool dma)
{
	if (dma) {
		// Pseudo-DMA read: hardware handshake for one byte.
		if (m_Bsy && m_Req && IsInput(m_Phase)) {
			u8 b = m_Data;
			m_Req = false;
			ByteTransferred(b);
			return b;
		}
		return m_Data;
	}

	switch (reg & 7) {
	case 0: // current SCSI data
		if (m_Mode & ModeArbitrate)
			return m_Odr;
		if (m_Bsy && IsInput(m_Phase))
			return m_Data;
		if (m_Icr & IcrData)
			return m_Odr;
		return 0;
	case 1:
		return (u8)((m_Icr & 0x9F) | ((m_Mode & ModeArbitrate) ? 0x40 : 0));
	case 2:
		return m_Mode;
	case 3:
		return m_Tcr;
	case 4:
		return BusStatus();
	case 5:
		return BusAndStatus();
	case 6: // input data
		return m_Data;
	case 7: // reset parity/interrupt
	default:
		m_Irq = false;
		return 0;
	}
}

void Scsi5380::Write(unsigned reg, u8 value, bool dma)
{
	if (dma) {
		if (m_Bsy && m_Req && !IsInput(m_Phase) && m_Phase != PhNone) {
			m_Req = false;
			ByteTransferred(value);
		}
		return;
	}

	switch (reg & 7) {
	case 0:
		m_Odr = value;
		break;
	case 1: {
		const u8 old = m_Icr;
		m_Icr = value & 0x9F;

		if (m_Icr & IcrRst) {
			BusFree();
			break;
		}

		// Selection: SEL asserted, BSY released, target ID on the data bus.
		if ((m_Icr & IcrSel) && !(m_Icr & IcrBsy) && m_Target < 0) {
			for (int id = kTargets - 1; id >= 0; --id)
				if ((m_Odr & (1u << id)) && m_Disks[id]) {
					m_Target = id;
					m_Bsy = true;
					m_Phase = PhNone;
					m_Req = false;
					break;
				}
		}

		// Selection complete once SEL is released.
		if (m_Target >= 0 && m_Phase == PhNone && (old & IcrSel) && !(m_Icr & IcrSel))
			EnterPhase((m_Icr & IcrAtn) ? PhMsgOut : PhCommand);

		// Manual REQ/ACK handshake.
		if (!(old & IcrAck) && (m_Icr & IcrAck) && m_Bsy && m_Req) {
			m_Latched = m_Odr;
			m_AckSeen = true;
			m_Req = false;
		}
		if ((old & IcrAck) && !(m_Icr & IcrAck) && m_AckSeen) {
			m_AckSeen = false;
			ByteTransferred(IsInput(m_Phase) ? m_Data : m_Latched);
		}
		break;
	}
	case 2:
		m_Mode = value;
		break;
	case 3:
		m_Tcr = value & 0x0F;
		break;
	case 4: // select enable
	case 5: // start DMA send
	case 6: // start DMA target receive
	case 7: // start DMA initiator receive
	default:
		break;
	}
}

void Scsi5380::ByteTransferred(u8 byte)
{
	switch (m_Phase) {
	case PhMsgOut:
		// IDENTIFY or other message: ignore, carry on with the command.
		EnterPhase(PhCommand);
		break;

	case PhCommand:
		if (m_CmdPos < sizeof m_Cmd)
			m_Cmd[m_CmdPos++] = byte;
		if (m_CmdPos == 1)
			m_CmdLen = CommandLength(byte);
		if (m_CmdPos >= m_CmdLen)
			ExecuteCommand();
		else
			m_Req = true;
		break;

	case PhDataIn:
		++m_BufPos;
		if (m_BufPos >= m_BufLen) {
			if (m_DiskTransfer && m_BlocksLeft > 0) {
				if (!LoadReadChunk()) {
					CheckCondition(kSenseMediumError, 0x11);
					break;
				}
			} else {
				FinishCommand(m_Status);
				break;
			}
		}
		PresentInputByte();
		m_Req = true;
		break;

	case PhDataOut:
		if (m_BufPos < m_BufLen)
			m_Buf[m_BufPos++] = byte;
		if (m_BufPos >= m_BufLen) {
			if (m_DiskTransfer) {
				if (!FlushWriteChunk()) {
					CheckCondition(kSenseMediumError, 0x0C);
					break;
				}
				if (m_BlocksLeft > 0) {
					u32 n = m_BlocksLeft < kChunkBlocks ? m_BlocksLeft : kChunkBlocks;
					m_BufLen = n * BlockDevice::kBlockSize;
					m_BufPos = 0;
					m_Req = true;
					break;
				}
			}
			FinishCommand(kStatusGood); // MODE SELECT data etc. is discarded
			break;
		}
		m_Req = true;
		break;

	case PhStatus:
		EnterPhase(PhMsgIn);
		break;

	case PhMsgIn:
		BusFree();
		break;

	default:
		break;
	}
}

void Scsi5380::FinishCommand(u8 status)
{
	m_DiskTransfer = false;
	m_Status = status;
	EnterPhase(PhStatus);
}

void Scsi5380::CheckCondition(u8 senseKey, u8 asc)
{
	m_SenseKey = senseKey;
	m_Asc = asc;
	FinishCommand(kStatusCheckCondition);
}

void Scsi5380::StartDataIn(unsigned length)
{
	m_BufLen = length;
	m_BufPos = 0;
	m_Status = kStatusGood;
	if (length == 0)
		FinishCommand(kStatusGood);
	else
		EnterPhase(PhDataIn);
}

bool Scsi5380::LoadReadChunk()
{
	BlockDevice *disk = m_Disks[m_Target];
	u32 n = m_BlocksLeft < kChunkBlocks ? m_BlocksLeft : kChunkBlocks;
	if (!disk->ReadBlocks(m_Lba, n, m_Buf)) {
		m_IoError = true;
		LogHex("scsi read error", m_Lba, n);
		return false;
	}
	m_Lba += n;
	m_BlocksLeft -= n;
	m_BlocksRead += n;
	m_BufLen = n * BlockDevice::kBlockSize;
	m_BufPos = 0;
	return true;
}

bool Scsi5380::FlushWriteChunk()
{
	BlockDevice *disk = m_Disks[m_Target];
	u32 n = m_BufLen / BlockDevice::kBlockSize;
	if (!disk->WriteBlocks(m_Lba, n, m_Buf)) {
		m_IoError = true;
		LogHex("scsi write error", m_Lba, n);
		return false;
	}
	m_Lba += n;
	m_BlocksLeft -= n;
	m_BlocksWritten += n;
	return true;
}

void Scsi5380::ExecuteCommand()
{
	BlockDevice *disk = m_Disks[m_Target];
	const u8 *c = m_Cmd;
	const u8 op = c[0];
	const u32 capacity = disk->BlockCount();
	const unsigned lun = c[1] >> 5;

	if (lun != 0 && op != 0x12 && op != 0x03) {
		CheckCondition(kSenseIllegalRequest, 0x25); // LUN not supported
		return;
	}

	switch (op) {
	case 0x00: // TEST UNIT READY
	case 0x04: // FORMAT UNIT
	case 0x1B: // START STOP UNIT
	case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
	case 0x2F: // VERIFY(10)
	case 0x35: // SYNCHRONIZE CACHE
	case 0x01: // REZERO UNIT
	case 0x0B: // SEEK(6)
	case 0x2B: // SEEK(10)
		FinishCommand(kStatusGood);
		return;

	case 0x03: { // REQUEST SENSE
		unsigned alloc = c[4] ? c[4] : 4;
		memset(m_Buf, 0, 18);
		m_Buf[0] = 0x70;
		m_Buf[2] = m_SenseKey;
		m_Buf[7] = 10;
		m_Buf[12] = m_Asc;
		m_SenseKey = kSenseNone;
		m_Asc = 0;
		StartDataIn(alloc < 18 ? alloc : 18);
		return;
	}

	case 0x12: { // INQUIRY
		unsigned alloc = c[4];
		memset(m_Buf, 0, 36);
		m_Buf[0] = lun ? 0x7F : 0x00; // direct-access device (or no LUN)
		m_Buf[2] = 0x01;              // SCSI-1 / CCS
		m_Buf[3] = 0x01;
		m_Buf[4] = 31;
		PutPadded(m_Buf + 8, "PIMAC", 8);
		PutPadded(m_Buf + 16, "VIRTUAL DISK", 16);
		PutPadded(m_Buf + 32, "1.0", 4);
		StartDataIn(alloc < 36 ? alloc : 36);
		return;
	}

	case 0x1A: { // MODE SENSE(6)
		unsigned alloc = c[4];
		memset(m_Buf, 0, 12);
		m_Buf[0] = 11;
		m_Buf[2] = disk->ReadOnly() ? 0x80 : 0x00;
		m_Buf[3] = 8; // one block descriptor
		m_Buf[5] = (u8)(capacity >> 16);
		m_Buf[6] = (u8)(capacity >> 8);
		m_Buf[7] = (u8)capacity;
		m_Buf[10] = 0x02; // 512-byte blocks
		StartDataIn(alloc < 12 ? alloc : 12);
		return;
	}

	case 0x15: // MODE SELECT(6): accept and discard the parameter list
		m_BufLen = c[4];
		m_BufPos = 0;
		m_DiskTransfer = false;
		if (m_BufLen == 0)
			FinishCommand(kStatusGood);
		else
			EnterPhase(PhDataOut);
		return;

	case 0x25: // READ CAPACITY
		PutBE32(m_Buf, capacity - 1);
		PutBE32(m_Buf + 4, BlockDevice::kBlockSize);
		StartDataIn(8);
		return;

	case 0x08:   // READ(6)
	case 0x28:   // READ(10)
	case 0x0A:   // WRITE(6)
	case 0x2A: { // WRITE(10)
		u32 lba, count;
		if (op == 0x08 || op == 0x0A) {
			lba = ((u32)(c[1] & 0x1F) << 16) | ((u32)c[2] << 8) | c[3];
			count = c[4] ? c[4] : 256;
		} else {
			lba = ((u32)c[2] << 24) | ((u32)c[3] << 16) | ((u32)c[4] << 8) | c[5];
			count = ((u32)c[7] << 8) | c[8];
		}
		if (count == 0) {
			FinishCommand(kStatusGood);
			return;
		}
		if (lba >= capacity || count > capacity - lba) {
			CheckCondition(kSenseIllegalRequest, kAscLbaOutOfRange);
			return;
		}
		m_DiskTransfer = true;
		m_Lba = lba;
		m_BlocksLeft = count;
		m_Status = kStatusGood;

		if (op == 0x08 || op == 0x28) {
			m_DiskWrite = false;
			if (!LoadReadChunk()) {
				CheckCondition(kSenseMediumError, 0x11);
				return;
			}
			EnterPhase(PhDataIn);
		} else {
			if (disk->ReadOnly()) {
				CheckCondition(kSenseDataProtect, kAscWriteProtected);
				return;
			}
			m_DiskWrite = true;
			u32 n = count < kChunkBlocks ? count : kChunkBlocks;
			m_BufLen = n * BlockDevice::kBlockSize;
			m_BufPos = 0;
			EnterPhase(PhDataOut);
		}
		return;
	}

	default:
		LogHex("scsi unsupported command", op, m_Target);
		CheckCondition(kSenseIllegalRequest, kAscInvalidOpcode);
		return;
	}

}

} // namespace pimac
