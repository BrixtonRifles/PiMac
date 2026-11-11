#pragma once

#include "core/compat.h"

namespace pimac {

// Which emulated machine runs a ROM (the ROM decides the model).
enum RomFamily {
	RomNotEmulated,
	RomCompact,  // Classic, SE, SE FDHD: mac/mac_compact
	RomClassic2, // Classic II: mac/mac_v8
	RomColorClassic, // Color Classic: mac/mac_v8 (Spice)
	RomColorClassic2, // Color Classic II: mac/mac_v8 (Sonora)
	RomSE30, // SE/30 (the II FDHD/IIx/IIcx ROM): mac/mac_se30, + its video ROM
	RomSE30Video, // the SE/30's 8 KB video declaration ROM: not a machine ROM
};

struct RomInfo {
	u32 checksum; // big-endian longword at offset 0
	const char *model;
	bool known;
	RomFamily family;
};

// Needs only the first 4 bytes.
RomInfo IdentifyRom(const u8 *data, unsigned size);

// True for a ROM that starts a Mac PiMac emulates (not one it doesn't, nor
// the SE/30 video ROM): what the menu's Machine setting offers.
inline bool IsMachineRom(RomFamily family)
{
	return family != RomNotEmulated && family != RomSE30Video;
}

// The ROM to use when none is chosen: the first that IsMachineRom, else
// the first that can be a machine's ROM (not the SE/30 video ROM). -1 if
// count is 0.
int DefaultRom(const RomInfo *infos, unsigned count);

// RAM sizes (KB, ascending) the family's machine supports; for
// RomNotEmulated, every size the settings allow.
const unsigned *RamOptionsKB(RomFamily family, unsigned &count);
// The supported size nearest to kb: the smallest one >= kb, else the largest.
unsigned FitRamKB(RomFamily family, unsigned kb);

// CPU speeds (percent of the real machine, ascending) the family offers:
// 100-150 on the Classic II / Color Classic, also 50 and 75 on the Color
// Classic II (its 33 MHz CPU needs about all of a Pi 3), 100 only otherwise.
const unsigned *CpuSpeedOptions(RomFamily family, unsigned &count);
// The offered speed nearest to percent (as FitRamKB).
unsigned FitCpuPercent(RomFamily family, unsigned percent);

} // namespace pimac
