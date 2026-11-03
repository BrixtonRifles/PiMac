#include "core/floppy_image.h"

namespace pimac {

static const unsigned k400K = 409600;
static const unsigned k800K = 819200;
static const unsigned k720K = 737280;
static const unsigned k1440K = 1474560;
static const unsigned kDc42Header = 84;

static u32 Be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void PutBe32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

// Disk Copy 4.2 checksum: add each big-endian word, rotate right by one.
static u32 Dc42Checksum(const u8 *p, unsigned bytes)
{
	u32 sum = 0;
	for (unsigned i = 0; i + 1 < bytes; i += 2) {
		sum += ((u32)p[i] << 8) | p[i + 1];
		sum = (sum >> 1) | (sum << 31);
	}
	return sum;
}

FloppyImage::FloppyImage()
	: m_File(0), m_FileSize(0), m_Format(None), m_DataOffset(0), m_TagOffset(0), m_Sides(0),
	  m_Blocks(0), m_Mfm(false), m_Writable(false), m_Dirty(false)
{
}

const char *FloppyImage::ResultText(Result r)
{
	switch (r) {
	case Ok:
		return "ok";
	case TooSmall:
		return "file too small";
	default:
		return "not a 400K, 800K, 720K or 1.44 MB disk image";
	}
}

FloppyImage::Result FloppyImage::Attach(u8 *file, unsigned size, bool writable)
{
	Detach();

	Format format = None;
	unsigned dataOffset = 0, dataSize = 0, tagOffset = 0;

	if (size >= kDc42Header && file[0] < 64 && file[0x52] == 0x01 && file[0x53] == 0x00) {
		// Disk Copy 4.2: name (Pascal string), data size, tag size, checksums...
		const u32 data = Be32(file + 0x40), tags = Be32(file + 0x44);
		if ((u64)kDc42Header + data + tags <= size) {
			format = DiskCopy42;
			dataOffset = kDc42Header;
			dataSize = data;
			if (tags == (data / 512) * 12)
				tagOffset = kDc42Header + data;
		}
	}
	if (format == None) {
		format = Raw;
		dataSize = size;
	}

	Result result = Ok;
	unsigned sides = 0;
	bool mfm = false;
	if (dataSize == k400K)
		sides = 1;
	else if (dataSize == k800K)
		sides = 2;
	else if (dataSize == k720K || dataSize == k1440K) {
		sides = 2;
		mfm = true;
		if (format == DiskCopy42)
			tagOffset = 0; // MFM sectors have no tags
	} else
		result = size < k400K ? TooSmall : UnknownSize;

	if (result != Ok) {
		delete[] file;
		return result;
	}

	m_File = file;
	m_FileSize = size;
	m_Format = format;
	m_DataOffset = dataOffset;
	m_TagOffset = tagOffset;
	m_Sides = sides;
	m_Blocks = dataSize / 512;
	m_Mfm = mfm;
	m_Writable = writable;
	m_Dirty = false;
	return Ok;
}

void FloppyImage::Detach()
{
	delete[] m_File;
	m_File = 0;
	m_FileSize = 0;
	m_Format = None;
	m_Sides = m_Blocks = 0;
	m_Mfm = false;
	m_Dirty = false;
}

const char *FloppyImage::SizeText() const
{
	if (m_Mfm)
		return HighDensity() ? "1.44 MB" : "720K";
	return m_Sides == 2 ? "800K" : "400K";
}

void FloppyImage::ReadSector(unsigned block, u8 *out) const
{
	if (block >= m_Blocks) {
		memset(out, 0, 524);
		return;
	}
	if (m_TagOffset)
		memcpy(out, m_File + m_TagOffset + block * 12, 12);
	else
		memset(out, 0, 12);
	memcpy(out + 12, m_File + m_DataOffset + block * 512, 512);
}

void FloppyImage::WriteSector(unsigned block, const u8 *in)
{
	if (block >= m_Blocks || !m_Writable)
		return;
	if (m_TagOffset)
		memcpy(m_File + m_TagOffset + block * 12, in, 12);
	memcpy(m_File + m_DataOffset + block * 512, in + 12, 512);
	m_Dirty = true;
}

void FloppyImage::PrepareToSave()
{
	if (m_Format == DiskCopy42) {
		PutBe32(m_File + 0x48, Dc42Checksum(m_File + m_DataOffset, m_Blocks * 512));
		if (m_TagOffset) // the first block's tags are not included
			PutBe32(m_File + 0x4C, Dc42Checksum(m_File + m_TagOffset + 12, (m_Blocks - 1) * 12));
	}
	m_Dirty = false;
}

} // namespace pimac
