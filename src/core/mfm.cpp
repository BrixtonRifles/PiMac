#include "core/mfm.h"

#include "core/floppy_image.h"

namespace pimac {
namespace mfm {

unsigned TrackBytes(bool highDensity)
{
	return highDensity ? kMaxTrackBytes : kMaxTrackBytes / 2;
}

u16 Crc(u16 crc, u8 byte)
{
	crc ^= (u16)(byte << 8);
	for (unsigned i = 0; i < 8; ++i)
		crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
	return crc;
}

namespace {

class TrackWriter {
public:
	TrackWriter(u8 *data, u8 *marks, unsigned len)
		: m_Data(data), m_Marks(marks), m_Len(len), m_Pos(0), m_Crc(0)
	{
	}
	void Put(u8 b, unsigned count = 1)
	{
		while (count-- && m_Pos < m_Len) {
			m_Data[m_Pos] = b;
			SetMark(m_Marks, m_Pos++, false);
			m_Crc = Crc(m_Crc, b);
		}
	}
	void Mark(u8 b)
	{
		for (unsigned i = 0; i < 3 && m_Pos < m_Len; ++i) {
			m_Data[m_Pos] = b;
			SetMark(m_Marks, m_Pos++, true);
		}
		m_Crc = kCrcAfterMarks;
	}
	void PutCrc()
	{
		const u16 crc = m_Crc;
		Put((u8)(crc >> 8));
		Put((u8)crc);
	}
	void Fill(u8 b) { Put(b, m_Len - m_Pos); }

private:
	u8 *m_Data, *m_Marks;
	unsigned m_Len, m_Pos;
	u16 m_Crc;
};

unsigned SectorBlock(const FloppyImage &image, unsigned track, unsigned head, unsigned sector)
{
	return (track * 2 + head) * image.MfmSectorsPerTrack() + sector - 1;
}

} // namespace

void BuildTrack(u8 *data, u8 *marks, unsigned len, unsigned track, unsigned head,
		const FloppyImage &image)
{
	const unsigned spt = image.MfmSectorsPerTrack();
	TrackWriter w(data, marks, len);
	u8 sector[524];

	w.Put(0x4E, 80);
	w.Put(0x00, 12);
	w.Mark(0xC2);
	w.Put(0xFC);
	w.Put(0x4E, 50);
	for (unsigned s = 1; s <= spt; ++s) {
		w.Put(0x00, 12);
		w.Mark(0xA1);
		w.Put(0xFE);
		w.Put((u8)track);
		w.Put((u8)head);
		w.Put((u8)s);
		w.Put(0x02); // 512 bytes
		w.PutCrc();
		w.Put(0x4E, 22);
		w.Put(0x00, 12);
		w.Mark(0xA1);
		w.Put(0xFB);
		image.ReadSector(SectorBlock(image, track, head, s), sector);
		for (unsigned i = 0; i < kSectorBytes; ++i)
			w.Put(sector[12 + i]);
		w.PutCrc();
		w.Put(0x4E, spt > 9 ? 84 : 80);
	}
	w.Fill(0x4E);
}

namespace {

// True if position i holds the last A1 mark before an `id` byte (FE: address
// field, FB/F8: data field).
bool FieldAt(const u8 *data, const u8 *marks, unsigned len, unsigned i, bool address)
{
	const unsigned n = (i + 1) % len;
	if (!IsMark(marks, i) || data[i] != 0xA1 || IsMark(marks, n))
		return false;
	return address ? data[n] == 0xFE : (data[n] == 0xFB || data[n] == 0xF8);
}

} // namespace

DecodeResult DecodeTrack(const u8 *data, const u8 *marks, unsigned len, unsigned track,
			 unsigned head, FloppyImage &image)
{
	DecodeResult r = {0, 0};
	const unsigned spt = image.MfmSectorsPerTrack();
	u8 sector[524], old[524];

	for (unsigned i = 0; i < len; ++i) {
		if (!FieldAt(data, marks, len, i, true))
			continue;
		// Address field: FE C H R N CRC CRC
		u8 id[7];
		u16 crc = kCrcAfterMarks;
		for (unsigned j = 0; j < 7; ++j) {
			id[j] = data[(i + 1 + j) % len];
			crc = Crc(crc, id[j]);
		}
		if (crc != 0) {
			++r.bad;
			continue;
		}
		const unsigned s = id[3];
		if (s < 1 || s > spt || id[4] != 2)
			continue;

		// Its data field follows within a gap's distance, before any other
		// address field.
		unsigned d = 0;
		bool found = false;
		for (d = 8; d < 80; ++d) {
			const unsigned k = (i + d) % len;
			if (FieldAt(data, marks, len, k, true))
				break;
			if (FieldAt(data, marks, len, k, false)) {
				found = true;
				break;
			}
		}
		if (!found)
			continue;
		const unsigned start = (i + d + 1) % len; // the FB byte
		crc = kCrcAfterMarks;
		for (unsigned j = 0; j < 1 + kSectorBytes + 2; ++j) {
			const u8 b = data[(start + j) % len];
			crc = Crc(crc, b);
			if (j >= 1 && j <= kSectorBytes)
				sector[12 + j - 1] = b;
		}
		if (crc != 0) {
			++r.bad;
			continue;
		}
		memset(sector, 0, 12);
		const unsigned block = SectorBlock(image, track, head, s);
		image.ReadSector(block, old);
		if (memcmp(old + 12, sector + 12, kSectorBytes) != 0) {
			image.WriteSector(block, sector);
			++r.written;
		}
	}
	return r;
}

} // namespace mfm
} // namespace pimac
