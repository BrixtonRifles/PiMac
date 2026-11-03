#include "core/vhd.h"

namespace pimac {

static u32 BE32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

VhdInfo ParseVhdFooter(const u8 footer[kVhdFooterSize])
{
	VhdInfo info;
	info.valid = memcmp(footer, "conectix", 8) == 0;
	info.diskType = BE32(footer + 60);
	info.currentSize = ((u64)BE32(footer + 48) << 32) | BE32(footer + 52);

	// Checksum: one's complement of the byte sum, excluding the field itself.
	u32 sum = 0;
	for (unsigned i = 0; i < kVhdFooterSize; ++i)
		if (i < 64 || i >= 68)
			sum += footer[i];
	info.checksumOk = info.valid && (u32)~sum == BE32(footer + 64);
	return info;
}

} // namespace pimac
