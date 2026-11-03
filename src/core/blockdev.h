// A disk made of 512-byte blocks (an image file on the SD card, or on the
// development machine for the host harness).
#pragma once

#include "core/compat.h"

namespace pimac {

class BlockDevice {
public:
	static const unsigned kBlockSize = 512;

	virtual u32 BlockCount() const = 0;
	virtual bool ReadBlocks(u32 lba, u32 count, u8 *buffer) = 0;
	virtual bool WriteBlocks(u32 lba, u32 count, const u8 *buffer) = 0;
	virtual bool ReadOnly() const { return false; }

protected:
	~BlockDevice() {}
};

} // namespace pimac
