#include "core/rom.h"
#include "core/settings.h"

namespace pimac {

namespace {

struct KnownRom {
	u32 checksum;
	const char *model;
	RomFamily family;
};

const KnownRom kKnownRoms[] = {
	{0x28BA61CE, "Macintosh 128K", RomNotEmulated},
	{0x28BA4E50, "Macintosh 512K", RomNotEmulated},
	{0x4D1EEEE1, "Macintosh Plus (v1)", RomNotEmulated},
	{0x4D1EEAE1, "Macintosh Plus (v2)", RomNotEmulated},
	{0x4D1F8172, "Macintosh Plus (v3)", RomNotEmulated},
	{0xB2E362A8, "Macintosh SE", RomCompact},
	{0xB306E171, "Macintosh SE FDHD", RomCompact},
	{0xA49F9914, "Macintosh Classic", RomCompact},
	{0x3193670E, "Macintosh Classic II", RomClassic2},
	{0xECD99DC0, "Macintosh Color Classic", RomColorClassic},
	// Also the LC 550, Performa 275/550/560 and Macintosh TV ROM: emulated as
	// the Color Classic II (the ROM picks the model from the display).
	{0xEDE66CBD, "Mac Color Classic II", RomColorClassic2},
	{0x97851DB6, "Macintosh II", RomNotEmulated},
	// Shared with the II FDHD, IIx and IIcx; emulated as the SE/30, which
	// also needs its video ROM (se30vrom) in /roms.
	{0x97221136, "Macintosh SE/30", RomSE30},
	// se30vrom.uk6: identified by its first longword (a declaration ROM has no
	// checksum there).
	{0x01000010, "SE/30 video ROM (used with the SE/30 ROM)", RomSE30Video},
	{0x368CADFE, "Macintosh IIci", RomNotEmulated},
};

} // namespace

RomInfo IdentifyRom(const u8 *data, unsigned size)
{
	RomInfo info;
	info.checksum = 0;
	info.model = "Unknown ROM";
	info.known = false;
	info.family = RomNotEmulated;
	if (!data || size < 4)
		return info;

	info.checksum = ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | data[3];
	for (unsigned i = 0; i < sizeof kKnownRoms / sizeof kKnownRoms[0]; ++i)
		if (kKnownRoms[i].checksum == info.checksum) {
			info.model = kKnownRoms[i].model;
			info.known = true;
			info.family = kKnownRoms[i].family;
		}
	return info;
}

int DefaultRom(const RomInfo *infos, unsigned count)
{
	if (!infos)
		return count ? 0 : -1;
	for (unsigned i = 0; i < count; ++i) // a machine PiMac emulates
		if (IsMachineRom(infos[i].family))
			return (int)i;
	for (unsigned i = 0; i < count; ++i) // else any machine ROM (shown as "not emulated")
		if (infos[i].family != RomSE30Video)
			return (int)i;
	return count ? 0 : -1;
}

const unsigned *RamOptionsKB(RomFamily family, unsigned &count)
{
	static const unsigned kCompact[] = {1024, 2048, 4096};
	static const unsigned kClassic2[] = {2048, 4096, 6144, 10240}; // 2 MB board + SIMMs
	static const unsigned kColorClassic[] = {4096, 6144, 8192, 10240}; // 4 MB board + SIMMs
	static const unsigned kColorClassic2[] = {4096, 6144, 8192, 12288, 20480, 36864}; // 4 MB + one SIMM
	static const unsigned kSE30[] = {4096, 5120, 8192, 16384, 20480, 32768, 65536, 131072};
	switch (family) {
	case RomCompact:
		count = sizeof kCompact / sizeof kCompact[0];
		return kCompact;
	case RomClassic2:
		count = sizeof kClassic2 / sizeof kClassic2[0];
		return kClassic2;
	case RomColorClassic:
		count = sizeof kColorClassic / sizeof kColorClassic[0];
		return kColorClassic;
	case RomColorClassic2:
		count = sizeof kColorClassic2 / sizeof kColorClassic2[0];
		return kColorClassic2;
	case RomSE30:
		count = sizeof kSE30 / sizeof kSE30[0];
		return kSE30;
	default:
		count = kRamOptionCount;
		return kRamOptionsKB;
	}
}

unsigned FitRamKB(RomFamily family, unsigned kb)
{
	unsigned count = 0;
	const unsigned *sizes = RamOptionsKB(family, count);
	for (unsigned i = 0; i < count; ++i)
		if (sizes[i] >= kb)
			return sizes[i];
	return sizes[count - 1];
}

const unsigned *CpuSpeedOptions(RomFamily family, unsigned &count)
{
	static const unsigned kFixed[] = {100};
	static const unsigned kFaster[] = {100, 125, 150};
	switch (family) {
	case RomClassic2:
	case RomColorClassic:
	case RomSE30:
		count = sizeof kFaster / sizeof kFaster[0];
		return kFaster;
	case RomColorClassic2:
		count = kCpuSpeedCount;
		return kCpuSpeeds;
	default:
		count = 1;
		return kFixed;
	}
}

unsigned FitCpuPercent(RomFamily family, unsigned percent)
{
	unsigned count = 0;
	const unsigned *speeds = CpuSpeedOptions(family, count);
	for (unsigned i = 0; i < count; ++i)
		if (speeds[i] >= percent)
			return speeds[i];
	return speeds[count - 1];
}

} // namespace pimac
