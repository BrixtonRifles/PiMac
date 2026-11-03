#include "host/pi_storage.h"

#include <fatfs/ff.h>

namespace PiStorage {

bool ReadWholeFile(const char *pPath, u8 **ppData, unsigned *pnSize, unsigned nMaxSize)
{
	FIL File;
	if (f_open(&File, pPath, FA_READ) != FR_OK)
		return false;

	FSIZE_t nSize = f_size(&File);
	if (nSize == 0 || nSize > nMaxSize) {
		f_close(&File);
		return false;
	}

	u8 *pData = new u8[(unsigned)nSize];
	UINT nRead = 0;
	bool bOK = f_read(&File, pData, (UINT)nSize, &nRead) == FR_OK && nRead == nSize;
	f_close(&File);

	if (!bOK) {
		delete[] pData;
		return false;
	}
	*ppData = pData;
	*pnSize = (unsigned)nSize;
	return true;
}

bool OverwriteFile(const char *pPath, const void *pData, unsigned nSize)
{
	FIL File;
	if (f_open(&File, pPath, FA_WRITE | FA_OPEN_EXISTING) != FR_OK)
		return false;
	UINT nWritten = 0;
	bool bOK = f_write(&File, pData, (UINT)nSize, &nWritten) == FR_OK && nWritten == nSize;
	return f_close(&File) == FR_OK && bOK;
}

bool IsReadOnly(const char *pPath)
{
	FILINFO Info;
	return f_stat(pPath, &Info) == FR_OK && (Info.fattrib & AM_RDO);
}

bool WriteWholeFile(const char *pPath, const void *pData, unsigned nSize)
{
	FIL File;
	if (f_open(&File, pPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return false;
	UINT nWritten = 0;
	bool bOK = f_write(&File, pData, nSize, &nWritten) == FR_OK && nWritten == nSize;
	return f_close(&File) == FR_OK && bOK; // f_close flushes to the card
}

bool ReadFileTail(const char *pPath, u8 *pBuffer, unsigned nBytes, u64 *pnFileSize)
{
	FIL File;
	if (f_open(&File, pPath, FA_READ) != FR_OK)
		return false;

	FSIZE_t nSize = f_size(&File);
	*pnFileSize = nSize;
	UINT nRead = 0;
	bool bOK = nSize >= nBytes && f_lseek(&File, nSize - nBytes) == FR_OK &&
		   f_read(&File, pBuffer, nBytes, &nRead) == FR_OK && nRead == nBytes;
	f_close(&File);
	return bOK;
}

void ListDirectory(const char *pPath, pimac::FileList &rList, const char *const *ppExtensions)
{
	pimac::FileListClear(rList);

	DIR Directory;
	FILINFO Info;
	if (f_opendir(&Directory, pPath) != FR_OK)
		return;

	while (f_readdir(&Directory, &Info) == FR_OK && Info.fname[0]) {
		if (Info.fattrib & (AM_DIR | AM_HID | AM_SYS))
			continue;
		if (Info.fname[0] == '.') // includes macOS "._" resource-fork droppings
			continue;

		bool bMatch = ppExtensions == 0;
		for (unsigned i = 0; !bMatch && ppExtensions[i]; ++i)
			bMatch = pimac::StrEndsWithNoCase(Info.fname, ppExtensions[i]);
		if (bMatch)
			pimac::FileListAdd(rList, Info.fname);
	}
	f_closedir(&Directory);
}

} // namespace PiStorage
