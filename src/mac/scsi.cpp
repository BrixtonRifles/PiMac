#include "mac/scsi.h"
#include "core/log.h"

namespace pimac {

namespace {

const u8 kStatusGood = 0x00;
const u8 kStatusCheckCondition = 0x02;

const u8 kSenseNone = 0x00;
const u8 kSenseNotReady = 0x02;
const u8 kSenseMediumError = 0x03;
const u8 kSenseIllegalRequest = 0x05;
const u8 kSenseUnitAttention = 0x06;
const u8 kSenseDataProtect = 0x07;

const u8 kAscInvalidOpcode = 0x20;
const u8 kAscLbaOutOfRange = 0x21;
const u8 kAscInvalidField = 0x24;
const u8 kAscLunNotSupported = 0x25;
const u8 kAscWriteProtected = 0x27;
const u8 kAscMediumChanged = 0x28;
const u8 kAscNoMedium = 0x3A;
const u8 kAscRemovalPrevented = 0x53;

// What Apple's CD-ROM driver checks: the product (one of its list; "1.9a"
// or later firmware) and the vendor mode page 0x30.
const char kCdVendor[] = "SONY";
const char kCdProduct[] = "CD-ROM CDU-8003A";
const char kCdRevision[] = "1.9a";
const char kCdApplePage[] = "APPLE COMPUTER, INC   "; // 22 bytes

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

Scsi5380::Scsi5380()
	: m_CdId(-1), m_CdMedia(0), m_CdBlockShift(2), m_CdUnitAttention(false), m_CdLocked(false),
	  m_CdEjected(false), m_ModeSelect(false), m_BlocksRead(0), m_BlocksWritten(0), m_IoError(false)
{
	for (unsigned i = 0; i < kTargets; ++i)
		m_Disks[i] = 0;
	Reset();
}

void Scsi5380::AttachCdrom(int id)
{
	m_CdId = id >= 0 && id < kTargets ? id : -1;
	m_CdMedia = 0;
	m_CdUnitAttention = m_CdLocked = m_CdEjected = false;
	m_CdBlockShift = 2;
}

void Scsi5380::InsertCdrom(BlockDevice *disc)
{
	m_CdMedia = disc;
	m_CdUnitAttention = disc != 0; // the Mac's driver notices the new disc
	m_CdLocked = false;
	m_CdEjected = false;
	m_CdBlockShift = 2;
}

void Scsi5380::EjectCdrom()
{
	if (m_Target >= 0 && m_Target == m_CdId)
		BusFree(); // a transfer from the old disc cannot continue
	m_CdMedia = 0;
	m_CdUnitAttention = false;
	m_CdLocked = false;
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
	m_DmaSend = false;
	m_SenseKey = kSenseNone;
	m_Asc = 0;
	m_CdBlockShift = 2; // the drive powers up with 2048-byte blocks
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
	m_ModeSelect = false;
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
	// In DMA mode, REQ with a phase that no longer matches TCR raises the
	// interrupt (BSR bit 4): how the Mac II-family ROMs see a blind DMA
	// transfer end (ROM 97221136 waits on it after a write, ~0x26B06).
	if ((m_Mode & ModeDma) && !PhaseMatch())
		m_Irq = true;
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
	// REQ. In DMA receive mode the 5380 answers the target's REQ with ACK by
	// itself and holds the byte for the CPU (DRQ), so the target has dropped
	// REQ: System 7.5's SCSI code (Mac II family) waits for that after each
	// pseudo-DMA read chunk.
	if (m_Req && !((m_Mode & ModeDma) && IsInput(m_Phase) && PhaseMatch()))
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
	// DRQ. Receiving: a byte is waiting. Sending: the output latch is empty
	// (always: bytes go straight to the target), also after the target has
	// left the data phase - the Mac II-family ROMs wait for DRQ after the last
	// byte of a blind write (ROM 97221136 ~0x26B06).
	if ((m_Mode & ModeDma) && ((m_Req && PhaseMatch()) || m_DmaSend))
		v |= 0x40;
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
				if ((m_Odr & (1u << id)) && Present(id)) {
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
		if (!(value & ModeDma))
			m_DmaSend = false;
		break;
	case 3:
		m_Tcr = value & 0x0F;
		break;
	case 5: // start DMA send
		m_DmaSend = true;
		break;
	case 7: // start DMA initiator receive
		m_DmaSend = false;
		break;
	case 4: // select enable
	case 6: // start DMA target receive
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
			if (m_ModeSelect)
				CdModeSelectDone();
			FinishCommand(kStatusGood); // a disk's MODE SELECT data is discarded
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
	m_ModeSelect = false;
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
	BlockDevice *disk = Device(m_Target);
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
	BlockDevice *disk = Device(m_Target);
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

void Scsi5380::RequestSense(unsigned alloc)
{
	if (!alloc)
		alloc = 4;
	memset(m_Buf, 0, 18);
	m_Buf[0] = 0x70;
	m_Buf[2] = m_SenseKey;
	m_Buf[7] = 10;
	m_Buf[12] = m_Asc;
	m_SenseKey = kSenseNone;
	m_Asc = 0;
	StartDataIn(alloc < 18 ? alloc : 18);
}

void Scsi5380::ExecuteCommand()
{
	if (m_Target == m_CdId) {
		ExecuteCdCommand();
		return;
	}
	BlockDevice *disk = m_Disks[m_Target];
	const u8 *c = m_Cmd;
	const u8 op = c[0];
	const u32 capacity = disk->BlockCount();
	const unsigned lun = c[1] >> 5;

	if (lun != 0 && op != 0x12 && op != 0x03) {
		CheckCondition(kSenseIllegalRequest, kAscLunNotSupported);
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

	case 0x03: // REQUEST SENSE
		RequestSense(c[4]);
		return;

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

// --- CD-ROM drive ------------------------------------------------------------

void Scsi5380::ExecuteCdCommand()
{
	const u8 *c = m_Cmd;
	const u8 op = c[0];
	const unsigned lun = c[1] >> 5;

	if (op == 0x12) { // INQUIRY: always answers
		const unsigned alloc = c[4];
		memset(m_Buf, 0, 36);
		m_Buf[0] = lun ? 0x7F : 0x05; // CD-ROM (or no LUN)
		m_Buf[1] = 0x80;              // removable
		m_Buf[2] = 0x02;              // SCSI-2
		m_Buf[3] = 0x02;
		m_Buf[4] = 31;
		PutPadded(m_Buf + 8, kCdVendor, 8);
		PutPadded(m_Buf + 16, kCdProduct, 16);
		PutPadded(m_Buf + 32, kCdRevision, 4);
		StartDataIn(alloc < 36 ? alloc : 36);
		return;
	}
	if (op == 0x03) {
		RequestSense(c[4]);
		return;
	}
	if (lun) {
		CheckCondition(kSenseIllegalRequest, kAscLunNotSupported);
		return;
	}
	if (m_CdUnitAttention) {
		m_CdUnitAttention = false;
		CheckCondition(kSenseUnitAttention, kAscMediumChanged);
		return;
	}
	if (!m_CdMedia) {
		// The drive itself still answers: Apple's driver identifies it with
		// MODE SENSE before any disc is in, then polls TEST UNIT READY.
		switch (op) {
		case 0x1A: // MODE SENSE(6)
			CdModeSense(c);
			return;
		case 0x15: // MODE SELECT(6)
			break;
		case 0x1B: // START STOP UNIT
		case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
			m_CdLocked = op == 0x1E && (c[4] & 1);
			FinishCommand(kStatusGood);
			return;
		default:
			CheckCondition(kSenseNotReady, kAscNoMedium);
			return;
		}
	}

	const u32 capacity = m_CdMedia ? m_CdMedia->BlockCount() >> m_CdBlockShift : 0; // CD blocks
	switch (op) {
	case 0x00: // TEST UNIT READY
	case 0x01: // REZERO UNIT
	case 0x0B: // SEEK(6)
	case 0x2B: // SEEK(10)
	case 0x2F: // VERIFY(10)
	case 0x35: // SYNCHRONIZE CACHE
		FinishCommand(kStatusGood);
		return;

	case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
		m_CdLocked = (c[4] & 1) != 0;
		FinishCommand(kStatusGood);
		return;

	case 0x1B: // START STOP UNIT: LoEj with Start clear ejects
		if ((c[4] & 3) == 2) {
			if (m_CdLocked) {
				CheckCondition(kSenseIllegalRequest, kAscRemovalPrevented);
				return;
			}
			m_CdMedia = 0;
			m_CdEjected = true;
		}
		FinishCommand(kStatusGood);
		return;

	case 0x25: // READ CAPACITY
		PutBE32(m_Buf, capacity ? capacity - 1 : 0);
		PutBE32(m_Buf + 4, BlockDevice::kBlockSize << m_CdBlockShift);
		StartDataIn(8);
		return;

	case 0x08:   // READ(6)
	case 0x28: { // READ(10)
		u32 lba, count;
		if (op == 0x08) {
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
		m_DiskWrite = false;
		m_Lba = lba << m_CdBlockShift;
		m_BlocksLeft = count << m_CdBlockShift;
		m_Status = kStatusGood;
		if (!LoadReadChunk()) {
			CheckCondition(kSenseMediumError, 0x11);
			return;
		}
		EnterPhase(PhDataIn);
		return;
	}

	case 0x0A: // WRITE(6)
	case 0x2A: // WRITE(10)
	case 0x2E: // WRITE AND VERIFY
	case 0x04: // FORMAT UNIT
		CheckCondition(kSenseDataProtect, kAscWriteProtected);
		return;

	case 0x1A: // MODE SENSE(6)
		CdModeSense(c);
		return;

	case 0x15: // MODE SELECT(6): only the block length is used
		m_BufLen = c[4]; // at most 255, well inside m_Buf
		m_BufPos = 0;
		m_DiskTransfer = false;
		if (m_BufLen == 0) {
			FinishCommand(kStatusGood);
			return;
		}
		EnterPhase(PhDataOut);
		m_ModeSelect = true; // after EnterPhase (which does not clear it)
		return;

	case 0x43: // READ TOC
		CdReadToc(c);
		return;

	default:
		LogHex("scsi cd unsupported command", op, m_Target);
		CheckCondition(kSenseIllegalRequest, kAscInvalidOpcode);
		return;
	}
}

void Scsi5380::CdModeSense(const u8 *c)
{
	const unsigned page = c[2] & 0x3F;
	const bool dbd = (c[1] & 0x08) != 0;
	const unsigned alloc = c[4];
	const u32 capacity = m_CdMedia ? m_CdMedia->BlockCount() >> m_CdBlockShift : 0;
	const u32 blockLen = BlockDevice::kBlockSize << m_CdBlockShift;

	u8 *p = m_Buf;
	memset(p, 0, 128);
	unsigned n = 4;
	if (!dbd) {
		p[3] = 8; // one block descriptor
		p[5] = (u8)(capacity >> 16);
		p[6] = (u8)(capacity >> 8);
		p[7] = (u8)capacity;
		p[9] = (u8)(blockLen >> 16);
		p[10] = (u8)(blockLen >> 8);
		p[11] = (u8)blockLen;
		n = 12;
	}
	const bool all = page == 0x3F;
	bool known = all;
	if (all || page == 0x01) { // error recovery
		p[n] = 0x01;
		p[n + 1] = 6;
		n += 8;
		known = true;
	}
	if (all || page == 0x0D) { // CD-ROM parameters: 60 s/min, 75 frames/s
		p[n] = 0x0D;
		p[n + 1] = 6;
		p[n + 5] = 60;
		p[n + 7] = 75;
		n += 8;
		known = true;
	}
	if (all || page == 0x0E) { // audio control: both channels, full volume
		p[n] = 0x0E;
		p[n + 1] = 14;
		p[n + 2] = 0x04;
		p[n + 8] = 0x01;
		p[n + 9] = 0xFF;
		p[n + 10] = 0x02;
		p[n + 11] = 0xFF;
		n += 16;
		known = true;
	}
	if (all || page == 0x30) { // Apple's: marks an Apple drive
		p[n] = 0x30;
		p[n + 1] = 22;
		memcpy(p + n + 2, kCdApplePage, 22);
		n += 24;
		known = true;
	}
	if (!known) {
		CheckCondition(kSenseIllegalRequest, kAscInvalidField);
		return;
	}
	p[0] = (u8)(n - 1);
	StartDataIn(alloc < n ? alloc : n);
}

void Scsi5380::CdModeSelectDone()
{
	// Header (4 bytes), then an optional 8-byte block descriptor whose last
	// three bytes are the block length: 512 or 2048.
	if (m_BufLen < 12 || m_Buf[3] < 8)
		return;
	const u32 len = ((u32)m_Buf[9] << 16) | ((u32)m_Buf[10] << 8) | m_Buf[11];
	if (len == 512)
		m_CdBlockShift = 0;
	else if (len == 2048)
		m_CdBlockShift = 2;
}

namespace {
u8 Bcd(u32 v)
{
	return (u8)(((v / 10) << 4) | (v % 10));
}

// An address as minutes/seconds/frames (75 frames/s; LBA 0 is 00:02:00),
// in binary or (raw TOC: Q sub-channel data) BCD.
void PutMsf(u8 *p, u32 lba, bool bcd = false)
{
	const u32 f = lba + 150;
	const u32 m = f / (75 * 60), s = (f / 75) % 60, fr = f % 75;
	p[0] = bcd ? Bcd(m) : (u8)m;
	p[1] = bcd ? Bcd(s) : (u8)s;
	p[2] = bcd ? Bcd(fr) : (u8)fr;
}
} // namespace

void Scsi5380::CdReadToc(const u8 *c)
{
	// One data track starting at 0, then the lead-out. SCSI-2 drives of
	// this era take the format in the top bits of the control byte: 0 the
	// TOC, 1 multisession information, 2 the raw TOC (Q sub-channel
	// entries: always MSF, and BCD as on the disc). Apple's driver asks for
	// all three and takes the disc's length from the raw TOC's lead-out
	// (point A2), reading it as BCD: in binary a disc looks shorter (reads
	// past the misread end fail with paramErr, -50).
	const bool msf = (c[1] & 0x02) != 0;
	const unsigned start = c[6];
	const unsigned alloc = ((unsigned)c[7] << 8) | c[8];
	const unsigned format = c[9] >> 6;
	const u32 leadOut = m_CdMedia->BlockCount() >> 2; // 2048-byte sectors
	u8 *p = m_Buf;
	memset(p, 0, 64);
	p[2] = 1; // first track (session)
	p[3] = 1; // last track (session)
	unsigned n = 4;

	if (format == 1) { // multisession: the first track of the last session
		p[n + 1] = 0x14;
		p[n + 2] = 1;
		if (msf)
			PutMsf(p + n + 5, 0);
		n += 8;
	} else if (format == 2) { // raw TOC: points A0, A1, A2, then track 1
		const u8 points[4] = {0xA0, 0xA1, 0xA2, 0x01};
		for (unsigned i = 0; i < 4; ++i, n += 11) {
			u8 *e = p + n;
			e[0] = 1;    // session
			e[1] = 0x14; // ADR 1, data track
			e[3] = points[i];
			if (points[i] == 0xA0)
				e[8] = 1; // first track; PSEC 0 = CD-ROM disc
			else if (points[i] == 0xA1)
				e[8] = 1; // last track
			else
				PutMsf(e + 8, points[i] == 0xA2 ? leadOut : 0, true);
		}
	} else {
		if (start > 1 && start != 0xAA) {
			CheckCondition(kSenseIllegalRequest, kAscInvalidField);
			return;
		}
		const unsigned tracks[2] = {1, 0xAA};
		const u32 addrs[2] = {0, leadOut};
		for (unsigned i = start == 0xAA ? 1 : 0; i < 2; ++i) {
			p[n + 1] = 0x14; // ADR 1, data track
			p[n + 2] = (u8)tracks[i];
			if (msf)
				PutMsf(p + n + 5, addrs[i]);
			else
				PutBE32(p + n + 4, addrs[i]);
			n += 8;
		}
	}
	p[0] = (u8)((n - 2) >> 8);
	p[1] = (u8)(n - 2);
	StartDataIn(alloc < n ? alloc : n);
}

} // namespace pimac
