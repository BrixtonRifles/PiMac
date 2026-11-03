// SD card file access via the FatFs addon. Paths include the drive prefix,
// e.g. PIMAC_DRIVE "/roms/Mac Classic.rom".
#pragma once

#include <circle/types.h>
#include "core/settings.h"

#define PIMAC_DRIVE "SD:"

namespace PiStorage {

// Allocates *ppData with new[]; the caller delete[]s it.
bool ReadWholeFile(const char *pPath, u8 **ppData, unsigned *pnSize, unsigned nMaxSize);
bool WriteWholeFile(const char *pPath, const void *pData, unsigned nSize);

// Overwrites an existing file from the start without truncating it first, so
// a power cut mid-write never leaves a zero-length file (for disk images,
// whose size never changes).
bool OverwriteFile(const char *pPath, const void *pData, unsigned nSize);

// True if the file has the FAT read-only attribute set.
bool IsReadOnly(const char *pPath);

// Reads the last nBytes of a file (e.g. a VHD footer) and reports the file size.
bool ReadFileTail(const char *pPath, u8 *pBuffer, unsigned nBytes, u64 *pnFileSize);

// Reads the first nBytes of a file (e.g. a ROM's checksum).
bool ReadFileHead(const char *pPath, u8 *pBuffer, unsigned nBytes);

// Lists regular, non-hidden files. ppExtensions is a 0-terminated list of
// suffixes such as ".vhd", or 0 to accept every file.
void ListDirectory(const char *pPath, pimac::FileList &rList, const char *const *ppExtensions);

} // namespace PiStorage
