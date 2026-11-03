// IBM-style MFM tracks for the SuperDrive: 720K (9 sectors per track, 250
// kbit/s) and 1.44 MB (18 sectors, 500 kbit/s), 80 tracks, 2 sides, 300 rpm.
//
// A track is held as the bytes the SWIM's ISM sees after MFM decoding, plus a
// flag per byte for "mark" bytes (A1/C2 written with a missing clock pulse,
// which ordinary data can never produce):
//   gap 4E..., 12x 00, C2 C2 C2* FC, gap 4E...         (index mark)
//   per sector:
//     12x 00, A1 A1 A1* FE, track, side, sector (1..n), 02, CRC hi, CRC lo
//     22x 4E, 12x 00, A1 A1 A1* FB, 512 data bytes, CRC hi, CRC lo, gap 4E...
//   (* = mark bytes)
// The CRC is CRC-CCITT (x^16+x^12+x^5+1), preset to FFFF before the three A1
// marks, which leaves CDB4 once they have passed.
#pragma once

#include "core/compat.h"

namespace pimac {

class FloppyImage;

namespace mfm {

static const unsigned kTracks = 80;
static const unsigned kSectorBytes = 512;
static const unsigned kMaxTrackBytes = 12500; // 500 kbit/s at 300 rpm
static const u16 kCrcAfterMarks = 0xCDB4;

unsigned TrackBytes(bool highDensity); // 12500 or 6250

u16 Crc(u16 crc, u8 byte);

inline bool IsMark(const u8 *marks, unsigned i)
{
	return (marks[i >> 3] >> (i & 7)) & 1;
}
inline void SetMark(u8 *marks, unsigned i, bool mark)
{
	if (mark)
		marks[i >> 3] |= (u8)(1u << (i & 7));
	else
		marks[i >> 3] &= (u8)~(1u << (i & 7));
}
inline unsigned MarkBytes(unsigned trackBytes)
{
	return (trackBytes + 7) / 8;
}

// Fills `len` bytes (and their mark flags) with one formatted track of an
// MFM image, as the Mac's formatter lays it out: sectors 1..n in order.
void BuildTrack(u8 *data, u8 *marks, unsigned len, unsigned track, unsigned head,
		const FloppyImage &image);

struct DecodeResult {
	unsigned written; // sectors that changed in the image
	unsigned bad;     // address or data fields with a CRC error
};
// Finds every intact sector (address field, then its data field) on a track
// ring of `len` bytes, fields may wrap past the end, and stores the data in
// the image.
DecodeResult DecodeTrack(const u8 *data, const u8 *marks, unsigned len, unsigned track,
			 unsigned head, FloppyImage &image);

} // namespace mfm
} // namespace pimac
