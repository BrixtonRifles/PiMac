// A floppy disk image held in memory: raw sector dumps (.dsk/.img/.image of
// exactly 400K or 800K) or Disk Copy 4.2 images (84-byte header, then data,
// then optional tags). The whole file stays in memory so it can be written
// back unchanged apart from the sectors the Mac modified.
#pragma once

#include "core/compat.h"

namespace pimac {

class FloppyImage {
public:
	enum Format { None, Raw, DiskCopy42 };
	enum Result { Ok, TooSmall, HighDensity, UnknownSize };

	FloppyImage();

	// Takes ownership of `file` (allocated with new[]). On failure the buffer
	// is freed and the image stays empty.
	Result Attach(u8 *file, unsigned size, bool writable);
	void Detach(); // frees the buffer

	bool Present() const { return m_Format != None; }
	unsigned Sides() const { return m_Sides; }
	unsigned Blocks() const { return m_Blocks; }
	bool Writable() const { return m_Writable; }
	u8 FormatByte() const { return m_Sides == 2 ? 0x22 : 0x02; }

	// 524 bytes: 12 tag bytes then 512 data bytes.
	void ReadSector(unsigned block, u8 *tagAndData) const;
	void WriteSector(unsigned block, const u8 *tagAndData);

	bool Dirty() const { return m_Dirty; }
	// Brings the file buffer up to date (Disk Copy checksums) and clears the
	// dirty flag; then File()/FileSize() can be written out.
	void PrepareToSave();
	const u8 *File() const { return m_File; }
	unsigned FileSize() const { return m_FileSize; }

	static const char *ResultText(Result r);

private:
	u8 *m_File;
	unsigned m_FileSize;
	Format m_Format;
	unsigned m_DataOffset;
	unsigned m_TagOffset; // 0 = no tags stored
	unsigned m_Sides;
	unsigned m_Blocks;
	bool m_Writable;
	bool m_Dirty;
};

} // namespace pimac
