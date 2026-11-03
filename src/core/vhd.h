// Microsoft Virtual Hard Disk (VHD) footer parsing.
// A *fixed* VHD is a raw disk image followed by a 512-byte footer, so it can
// be used directly. Dynamic VHDs need block-allocation-table support (TODO).
#pragma once

#include "core/compat.h"

namespace pimac {

enum : u32 { VhdFixed = 2, VhdDynamic = 3, VhdDifferencing = 4 };

struct VhdInfo {
	bool valid;      // "conectix" cookie present
	bool checksumOk;
	u32 diskType;
	u64 currentSize; // size of the emulated disk in bytes
};

static const unsigned kVhdFooterSize = 512;

VhdInfo ParseVhdFooter(const u8 footer[kVhdFooterSize]);

} // namespace pimac
