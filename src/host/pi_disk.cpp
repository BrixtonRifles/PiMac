#include "host/pi_disk.h"

#include <circle/logger.h>
#include <circle/util.h>

static const char FromDisk[] = "disk";

PiDiskImage::PiDiskImage()
	: m_bOpen(false), m_bReadOnly(false), m_bDirty(false), m_nBlocks(0), m_pLinkMap(0)
{
}

PiDiskImage::~PiDiskImage()
{
	Close();
}

bool PiDiskImage::Open(const char *pPath, bool bReadOnly)
{
	Close();

	m_bReadOnly = bReadOnly;
	if (bReadOnly) {
		if (f_open(&m_File, pPath, FA_READ) != FR_OK)
			return false;
	} else if (f_open(&m_File, pPath, FA_READ | FA_WRITE) != FR_OK) {
		if (f_open(&m_File, pPath, FA_READ) != FR_OK)
			return false;
		m_bReadOnly = true;
	}
	m_bOpen = true;

	FSIZE_t nSize = f_size(&m_File);

	// A fixed VHD ends with a 512-byte footer that is not part of the disk.
	if (nSize >= 1024) {
		u8 Footer[512];
		UINT nRead = 0;
		if (f_lseek(&m_File, nSize - 512) == FR_OK &&
		    f_read(&m_File, Footer, 512, &nRead) == FR_OK && nRead == 512 &&
		    memcmp(Footer, "conectix", 8) == 0)
			nSize -= 512;
	}
	m_nBlocks = (u32)(nSize / kBlockSize);

#if FF_USE_FASTSEEK
	// Build the cluster link map for fast seeking. Start with room for a
	// lightly fragmented file and grow if FatFs says it needs more.
	DWORD nEntries = 256;
	for (int nTry = 0; nTry < 2; ++nTry) {
		m_pLinkMap = new DWORD[nEntries];
		m_pLinkMap[0] = nEntries;
		m_File.cltbl = m_pLinkMap;
		FRESULT Result = f_lseek(&m_File, CREATE_LINKMAP);
		if (Result == FR_OK)
			break;
		DWORD nNeeded = m_pLinkMap[0];
		m_File.cltbl = 0;
		delete[] m_pLinkMap;
		m_pLinkMap = 0;
		if (Result != FR_NOT_ENOUGH_CORE)
			break;
		nEntries = nNeeded;
	}
#endif

	CLogger::Get()->Write(FromDisk, LogNotice, "%s: %u blocks%s%s", pPath, m_nBlocks,
			      m_bReadOnly ? ", read-only" : "",
			      m_pLinkMap ? ", fast seek" : ", SLOW seek (no link map)");
	return true;
}

void PiDiskImage::Close()
{
	if (m_bOpen) {
		f_close(&m_File); // flushes
		m_bOpen = false;
	}
	m_bDirty = false;
	delete[] m_pLinkMap;
	m_pLinkMap = 0;
	m_nBlocks = 0;
}

bool PiDiskImage::Seek(u32 lba)
{
	return f_lseek(&m_File, (FSIZE_t)lba * kBlockSize) == FR_OK;
}

bool PiDiskImage::ReadBlocks(u32 lba, u32 count, u8 *buffer)
{
	if (!m_bOpen || lba >= m_nBlocks || count > m_nBlocks - lba || !Seek(lba))
		return false;
	UINT nBytes = count * kBlockSize, nRead = 0;
	return f_read(&m_File, buffer, nBytes, &nRead) == FR_OK && nRead == nBytes;
}

bool PiDiskImage::WriteBlocks(u32 lba, u32 count, const u8 *buffer)
{
	if (!m_bOpen || m_bReadOnly || lba >= m_nBlocks || count > m_nBlocks - lba || !Seek(lba))
		return false;
	UINT nBytes = count * kBlockSize, nWritten = 0;
	m_bDirty = true;
	return f_write(&m_File, buffer, nBytes, &nWritten) == FR_OK && nWritten == nBytes;
}

void PiDiskImage::Sync()
{
	if (m_bOpen && m_bDirty) {
		f_sync(&m_File);
		m_bDirty = false;
	}
}
