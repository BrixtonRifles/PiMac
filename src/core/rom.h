#pragma once

#include "core/compat.h"

namespace pimac {

struct RomInfo {
	u32 checksum; // big-endian longword at offset 0
	const char *model;
	bool known;
};

RomInfo IdentifyRom(const u8 *data, unsigned size);

} // namespace pimac
