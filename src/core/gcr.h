// Macintosh 400K/800K floppy format: GCR "6 and 2" encoding of sectors as
// the Sony driver in ROM reads and writes them.
//
// A track is a sequence of sectors, each an address field followed by a data
// field, separated by self-sync 0xFF bytes:
//   address: D5 AA 96, track lo6, sector, side|track hi, format, checksum, DE AA
//   data:    D5 AA AD, sector, 699 nibbles (12 tag + 512 data bytes), 4
//            checksum nibbles, DE AA
// Tracks are in 5 zones of 16; zone 0 (outermost) has 12 sectors per track,
// zone 4 has 8. Reference: Clock Signal's AppleGCR encoder (MIT licence),
// itself cross-referenced with the Mac Plus ROM.
#pragma once

#include "core/compat.h"

namespace pimac {
namespace gcr {

static const unsigned kTracks = 80;
static const unsigned kTagBytes = 12;
static const unsigned kSectorBytes = 512;
static const unsigned kAddressNibbles = 10;
static const unsigned kDataNibbles = 709;

unsigned SectorsPerTrack(unsigned track);
// First logical block of `track` on a disk with `sides` sides (1 or 2).
// Blocks run track by track, side 0 then side 1, sector 0 upwards.
unsigned TrackStartBlock(unsigned track, unsigned sides);

u8 Encode6(u8 value);  // 6-bit value -> disk nibble
int Decode6(u8 nibble); // -1 if not a valid data nibble

// Writes 10 nibbles. format: 0x22 for double-sided 800K, 0x02 single-sided.
void EncodeAddress(u8 *out, unsigned track, unsigned sector, unsigned side, u8 format);
// Writes 709 nibbles for 524 bytes (12 tag bytes then 512 data bytes).
void EncodeData(u8 *out, unsigned sector, const u8 *tagAndData);
// Decodes the 703 nibbles that follow "D5 AA AD sector": 699 data and 4
// checksum. Returns false on an invalid nibble or checksum mismatch.
bool DecodeData(const u8 *nibbles, u8 *tagAndData);

} // namespace gcr
} // namespace pimac
