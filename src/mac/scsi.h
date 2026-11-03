// NCR 5380 SCSI controller plus emulated SCSI hard disks (direct-access
// targets backed by BlockDevices). Targets respond instantly; the Mac's
// polled and pseudo-DMA ("blind") transfers are both supported.
#pragma once

#include "core/blockdev.h"
#include "core/compat.h"

namespace pimac {

class Scsi5380 {
public:
	enum { kTargets = 7, kChunkBlocks = 32 };

	Scsi5380();

	void Attach(unsigned id, BlockDevice *disk);
	void Reset();

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
	void FinishCommand(u8 status);
	void CheckCondition(u8 senseKey, u8 asc);
	void StartDataIn(unsigned length);
	bool LoadReadChunk();
	bool FlushWriteChunk();
	bool PhaseMatch() const;
	u8 BusStatus() const;
	u8 BusAndStatus() const;

	BlockDevice *m_Disks[kTargets];

	// 5380 registers
	u8 m_Odr, m_Icr, m_Mode, m_Tcr;
	bool m_Irq;

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
