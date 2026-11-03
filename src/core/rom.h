#pragma once

#include "core/compat.h"

namespace pimac {

// Which emulated machine runs a ROM (the ROM decides the model).
enum RomFamily {
	RomNotEmulated,
	RomCompact,  // Classic, SE, SE FDHD: mac/mac_compact
	RomClassic2, // Classic II: mac/mac_v8
	RomColorClassic, // Color Classic: mac/mac_v8 (Spice)
};

struct RomInfo {
	u32 checksum; // big-endian longword at offset 0
	const char *model;
	bool known;
	RomFamily family;
};

// Needs only the first 4 bytes.
RomInfo IdentifyRom(const u8 *data, unsigned size);

// RAM sizes (KB, ascending) the family's machine supports; for
// RomNotEmulated, every size the settings allow.
const unsigned *RamOptionsKB(RomFamily family, unsigned &count);
// The supported size nearest to kb: the smallest one >= kb, else the largest.
unsigned FitRamKB(RomFamily family, unsigned kb);

} // namespace pimac
