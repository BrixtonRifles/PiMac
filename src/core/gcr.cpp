#include "core/gcr.h"

namespace pimac {
namespace gcr {

static const u8 kSixAndTwo[64] = {
	0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6, 0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
	0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
	0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
	0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

unsigned SectorsPerTrack(unsigned track)
{
	return 12 - (track >> 4);
}

unsigned TrackStartBlock(unsigned track, unsigned sides)
{
	unsigned block = 0;
	for (unsigned t = 0; t < track; ++t)
		block += SectorsPerTrack(t) * sides;
	return block;
}

u8 Encode6(u8 value)
{
	return kSixAndTwo[value & 0x3F];
}

int Decode6(u8 nibble)
{
	// The table is sorted, so a binary search will do.
	int lo = 0, hi = 63;
	while (lo <= hi) {
		const int mid = (lo + hi) / 2;
		if (kSixAndTwo[mid] == nibble)
			return mid;
		if (kSixAndTwo[mid] < nibble)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

void EncodeAddress(u8 *out, unsigned track, unsigned sector, unsigned side, u8 format)
{
	const u8 f0 = (u8)(track & 0x3F);
	const u8 f1 = (u8)(sector & 0x3F);
	const u8 f2 = (u8)((side ? 0x20 : 0x00) | ((track >> 6) & 0x1F));
	const u8 f3 = (u8)(format & 0x3F);
	out[0] = 0xD5;
	out[1] = 0xAA;
	out[2] = 0x96;
	out[3] = Encode6(f0);
	out[4] = Encode6(f1);
	out[5] = Encode6(f2);
	out[6] = Encode6(f3);
	out[7] = Encode6((u8)(f0 ^ f1 ^ f2 ^ f3));
	out[8] = 0xDE;
	out[9] = 0xAA;
}

// The Mac's running checksum: three byte-wide sums, one per position in each
// group of three bytes, with carries passed round from one to the next
// (mirrors the ROL/ADDX/EOR sequence in the ROM).
struct Checksum {
	unsigned c0, c1, c2;
};

void EncodeData(u8 *out, unsigned sector, const u8 *src)
{
	Checksum k = {0, 0, 0};
	out[0] = 0xD5;
	out[1] = 0xAA;
	out[2] = 0xAD;
	out[3] = Encode6((u8)sector);

	u8 *n = out + 4;
	for (unsigned g = 0; g < 175; ++g) {
		k.c0 = (k.c0 & 0xFF);
		k.c0 = ((k.c0 << 1) | (k.c0 >> 7)) & 0xFF; // ROL.B

		const u8 v0 = (u8)(*src ^ k.c0);
		k.c2 += *src++ + (k.c0 & 1);
		const u8 v1 = (u8)(*src ^ k.c2);
		k.c1 += *src++ + (k.c2 >> 8);
		u8 v2 = 0;
		if (g < 174) {
			v2 = (u8)(*src ^ k.c1);
			k.c0 += *src++ + (k.c1 >> 8);
		}
		k.c1 &= 0xFF;
		k.c2 &= 0xFF;

		n[0] = Encode6((u8)(((v0 >> 2) & 0x30) | ((v1 >> 4) & 0x0C) | ((v2 >> 6) & 0x03)));
		n[1] = Encode6(v0);
		n[2] = Encode6(v1);
		if (g < 174)
			n[3] = Encode6(v2);
		n += 4;
	}
	// The last group has only two bytes, so the checksum starts one early.
	n = out + 4 + 699;
	n[0] = Encode6((u8)(((k.c2 >> 2) & 0x30) | ((k.c1 >> 4) & 0x0C) | ((k.c0 >> 6) & 0x03)));
	n[1] = Encode6((u8)k.c2);
	n[2] = Encode6((u8)k.c1);
	n[3] = Encode6((u8)k.c0);
	n[4] = 0xDE;
	n[5] = 0xAA;
}

bool DecodeData(const u8 *n, u8 *dst)
{
	Checksum k = {0, 0, 0};
	for (unsigned g = 0; g < 175; ++g) {
		const int hi = Decode6(n[0]);
		const int l0 = Decode6(n[1]);
		const int l1 = Decode6(n[2]);
		const int l2 = g < 174 ? Decode6(n[3]) : 0;
		if (hi < 0 || l0 < 0 || l1 < 0 || l2 < 0)
			return false;
		const u8 v0 = (u8)(l0 | ((hi << 2) & 0xC0));
		const u8 v1 = (u8)(l1 | ((hi << 4) & 0xC0));
		const u8 v2 = (u8)(l2 | ((hi << 6) & 0xC0));

		k.c0 = (k.c0 & 0xFF);
		k.c0 = ((k.c0 << 1) | (k.c0 >> 7)) & 0xFF;

		const u8 s0 = (u8)(v0 ^ k.c0);
		*dst++ = s0;
		k.c2 += s0 + (k.c0 & 1);
		const u8 s1 = (u8)(v1 ^ k.c2);
		*dst++ = s1;
		k.c1 += s1 + (k.c2 >> 8);
		if (g < 174) {
			const u8 s2 = (u8)(v2 ^ k.c1);
			*dst++ = s2;
			k.c0 += s2 + (k.c1 >> 8);
		}
		k.c1 &= 0xFF;
		k.c2 &= 0xFF;
		n += 4;
	}
	n -= 1; // last group was three nibbles
	const int hi = Decode6(n[0]);
	const int a = Decode6(n[1]), b = Decode6(n[2]), c = Decode6(n[3]);
	if (hi < 0 || a < 0 || b < 0 || c < 0)
		return false;
	const unsigned c2 = (unsigned)(a | ((hi << 2) & 0xC0));
	const unsigned c1 = (unsigned)(b | ((hi << 4) & 0xC0));
	const unsigned c0 = (unsigned)(c | ((hi << 6) & 0xC0));
	return c2 == (k.c2 & 0xFF) && c1 == (k.c1 & 0xFF) && c0 == (k.c0 & 0xFF);
}

} // namespace gcr
} // namespace pimac
