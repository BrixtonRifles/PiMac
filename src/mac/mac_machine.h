// What the host (kernel, hostmac) needs from any emulated Mac model.
#pragma once

#include "core/blockdev.h"
#include "core/machine.h"
#include "mac/iwm.h"
#include "mac/rtc.h"
#include "mac/scc.h"
#include "mac/scsi.h"

namespace pimac {

struct MacConfig {
	unsigned ramKB;   // the model's own choices (see each Start())
	const u8 *rom;    // caller keeps this alive
	unsigned romSize; // power of two
	BlockDevice *disk; // SCSI ID 0, may be 0
	u32 clockSeconds; // initial clock (seconds since 1904)
};

class MacMachine : public Machine {
public:
	// Returns false if the configuration is unusable (message in Error()).
	virtual bool Start(const MacConfig &config) = 0;
	virtual const char *Error() const = 0;

	virtual Rtc &GetRtc() = 0; // PRAM + clock (persisted by the host)
	virtual const Scsi5380 &GetScsi() const = 0;
	virtual Scsi5380 &GetScsi() = 0; // CD-ROM: InsertCdrom/EjectCdrom/TakeCdromEjected
	virtual Iwm &GetIwm() = 0; // floppy drives: Insert/Remove/TakeEjected
	virtual Scc &GetScc() = 0; // serial ports: Attach
	virtual bool Overlay() const = 0;
	virtual u8 *Ram() = 0; // for dumps
	virtual u32 RamSize() const = 0;

protected:
	~MacMachine() {}
};

} // namespace pimac
