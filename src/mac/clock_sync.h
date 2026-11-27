// Setting a Mac's clock while it runs (network time).
#pragma once

#include "mac/mac_machine.h"

namespace pimac {

enum class ClockSync { Unchanged, Chip, ChipAndSystem };

// Brings the Mac's clock to `macSeconds` (local time, seconds since 1904),
// changing only what is 2 s or more off. The clock chip is set; but Mac OS
// reads the chip only at startup, into the low-memory global Time ($20C),
// and then counts seconds there itself (losing some while it starts up), so
// that copy is corrected too - but only when it holds a time close to what
// the chip held (at most a day behind, a minute ahead): that shows the
// system is running and keeping its time there. Before then (the ROM's
// memory test, early startup) RAM is left alone, and the system reads the
// new time from the chip when it gets that far.
ClockSync SyncMacClock(MacMachine &mac, u32 macSeconds);

} // namespace pimac
