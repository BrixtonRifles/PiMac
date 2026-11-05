// NCR 5380 SCSI controller plus emulated SCSI hard disks (direct-access
// targets backed by BlockDevices) and one CD-ROM drive. Targets respond
// instantly; the Mac's polled and pseudo-DMA ("blind") transfers are both
// supported.
//
// The CD-ROM drive identifies as an AppleCD 300 (Sony CDU-8003A, firmware
// 1.9a, with Apple's mode page 0x30), the drives Apple's CD-ROM driver
// accepts. It reads a disc image (.toast/.iso: 2048-byte sectors, as
// 512-byte BlockDevice blocks) in 2048- or 512-byte blocks, as the Mac sets
// with MODE SELECT. No disc: NOT READY; a new disc: one UNIT ATTENTION.
#pragma once

#include "core/blockdev.h"
#include "core/compat.h"

namespace pimac {

class Scsi5380 {
public:
	enum { kTargets = 7, kChunkBlocks = 32 };

	Scsi5380();

	void Attach(unsigned id, BlockDevice *disk); // hard disk (0 = none)
	void Reset();

	// CD-ROM drive: present at `id` (empty) until AttachCdrom(-1).
	void AttachCdrom(int id);
	void InsertCdrom(BlockDevice *disc); // read-only use; caller keeps it alive
	void EjectCdrom();                   // host side
	bool CdromLoaded() const { return m_CdMedia != 0; }
	// True once after the Mac ejected the disc (the caller may close it).
	bool TakeCdromEjected()
	{
		const bool e = m_CdEjected;
		m_CdEjected = false;
		return e;
	}

	u8 Read(unsigned reg, bool dma);
	void Write(unsigned reg, u8 value, bool dma);

	// Statistics for diagnostics
	u32 BlocksRead() const { return m_BlocksRead; }
	u32 BlocksWritten() const { return m_BlocksWritten; }
	bool IoError() const { return m_IoError; }

private:
	enum : u8 {
		PhDataOut = 0,
		PhDataIn = 1,
		PhCommand = 2,
		PhStatus = 3,
		PhMsgOut = 6,
		PhMsgIn = 7,
		PhNone = 0xFF
	};
	enum : u8 { IcrData = 0x01, IcrAtn = 0x02, IcrSel = 0x04, IcrBsy = 0x08, IcrAck = 0x10, IcrRst = 0x80 };
	enum : u8 { ModeArbitrate = 0x01, ModeDma = 0x02 };

	static bool IsInput(u8 phase) { return phase == PhDataIn || phase == PhStatus || phase == PhMsgIn; }

	void BusFree();
	void EnterPhase(u8 phase);
	void PresentInputByte();
	void ByteTransferred(u8 byte); // REQ/ACK cycle done; byte is meaningful for output phases
	void ExecuteCommand();
	void ExecuteCdCommand();
	void RequestSense(unsigned alloc);
	void CdModeSense(const u8 *c);
	void CdReadToc(const u8 *c);
	void CdModeSelectDone();
	BlockDevice *Device(int id) const { return id == m_CdId ? m_CdMedia : m_Disks[id]; }
	bool Present(int id) const { return m_Disks[id] || id == m_CdId; }
	void FinishCommand(u8 status);
	void CheckCondition(u8 senseKey, u8 asc);
	void StartDataIn(unsigned length);
	bool LoadReadChunk();
	bool FlushWriteChunk();
	bool PhaseMatch() const;
	u8 BusStatus() const;
	u8 BusAndStatus() const;

	BlockDevice *m_Disks[kTargets];

	// CD-ROM drive
	int m_CdId; // -1: none
	BlockDevice *m_CdMedia;
	unsigned m_CdBlockShift; // CD block = 512 << shift bytes (2: 2048)
	bool m_CdUnitAttention;
	bool m_CdLocked; // PREVENT MEDIUM REMOVAL
	bool m_CdEjected;
	bool m_ModeSelect; // MODE SELECT data arriving for the CD

	// 5380 registers
	u8 m_Odr, m_Icr, m_Mode, m_Tcr;
	bool m_Irq;
	bool m_DmaSend; // "start DMA send" (reg 5) until DMA mode ends

	// Bus / target state
	int m_Target;
	bool m_Bsy;
	bool m_Req;
	u8 m_Phase;
	u8 m_Data;      // byte the target drives in input phases
	bool m_AckSeen; // initiator asserted ACK for the current byte
	u8 m_Latched;   // byte latched from the initiator

	u8 m_Cmd[16];
	unsigned m_CmdLen, m_CmdPos;

	u8 m_Buf[kChunkBlocks * BlockDevice::kBlockSize];
	unsigned m_BufLen, m_BufPos;

	// Block transfer in progress
	bool m_DiskTransfer;
	bool m_DiskWrite;
	u32 m_Lba;
	u32 m_BlocksLeft;

	u8 m_Status;
	u8 m_SenseKey, m_Asc;

	u32 m_BlocksRead, m_BlocksWritten;
	bool m_IoError;
};

} // namespace pimac
