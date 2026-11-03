#include "core/rom.h"

namespace pimac {

namespace {

struct KnownRom {
	u32 checksum;
	const char *model;
};

const KnownRom kKnownRoms[] = {
	{0x28BA61CE, "Macintosh 128K"},
	{0x28BA4E50, "Macintosh 512K"},
	{0x4D1EEEE1, "Macintosh Plus (v1)"},
	{0x4D1EEAE1, "Macintosh Plus (v2)"},
	{0x4D1F8172, "Macintosh Plus (v3)"},
	{0xB2E362A8, "Macintosh SE"},
	{0xB306E171, "Macintosh SE FDHD"},
	{0xA49F9914, "Macintosh Classic"},
	{0x97851DB6, "Macintosh II"},
	{0x97221136, "Macintosh IIx/IIcx/SE/30"},
	{0x368CADFE, "Macintosh IIci"},
};

} // namespace

RomInfo IdentifyRom(const u8 *data, unsigned size)
{
	RomInfo info;
	info.checksum = 0;
	info.model = "Unknown ROM";
	info.known = false;
	if (!data || size < 4)
		return info;

	info.checksum = ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | data[3];
	for (unsigned i = 0; i < sizeof kKnownRoms / sizeof kKnownRoms[0]; ++i)
		if (kKnownRoms[i].checksum == info.checksum) {
			info.model = kKnownRoms[i].model;
			info.known = true;
		}
	return info;
}

} // namespace pimac
