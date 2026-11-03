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
	{0x97851DB6, "Macintosh II", RomNotEmulated},
	{0x97221136, "Macintosh IIx/IIcx/SE/30", RomNotEmulated},
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

const unsigned *RamOptionsKB(RomFamily family, unsigned &count)
{
	static const unsigned kCompact[] = {1024, 2048, 4096};
	static const unsigned kClassic2[] = {2048, 4096, 6144, 10240}; // 2 MB board + SIMMs
	static const unsigned kColorClassic[] = {4096, 6144, 8192, 10240}; // 4 MB board + SIMMs
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

} // namespace pimac
