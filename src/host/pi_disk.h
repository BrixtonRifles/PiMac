// A disk image file on the SD card used as a BlockDevice. Uses FatFs fast
// seek (cluster link map), so random access into multi-GB images is quick.
// Writes go straight to the file; Sync() flushes FatFs buffers to the card.
#pragma once

#include <fatfs/ff.h>
#include "core/blockdev.h"

class PiDiskImage : public pimac::BlockDevice {
public:
	PiDiskImage();
	~PiDiskImage();

	bool Open(const char *pPath, bool bReadOnly = false); // read-only: CD-ROM images
	void Close();
	bool IsOpen() const { return m_bOpen; }

	u32 BlockCount() const override { return m_nBlocks; }
	bool ReadBlocks(u32 lba, u32 count, u8 *buffer) override;
	bool WriteBlocks(u32 lba, u32 count, const u8 *buffer) override;
	bool ReadOnly() const override { return m_bReadOnly; }

	// Flush pending writes; cheap when nothing changed.
	void Sync();
	bool FastSeek() const { return m_pLinkMap != 0; }

private:
	bool Seek(u32 lba);

	FIL m_File;
	bool m_bOpen;
	bool m_bReadOnly;
	bool m_bDirty;
	u32 m_nBlocks;
	DWORD *m_pLinkMap;
};
