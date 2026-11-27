#include "mac/clock_sync.h"

#include "mac/cpu.h"

namespace pimac {

static const u32 kTime = 0x20C; // low-memory global: seconds since 1904

static bool Near(u32 a, u32 b)
{
	return (a > b ? a - b : b - a) < 2;
}

ClockSync SyncMacClock(MacMachine &mac, u32 macSeconds)
{
	Rtc &rtc = mac.GetRtc();
	const u32 chip = rtc.Seconds();
	u32 time = 0;
	bool system = !mac.Overlay() && cpu::PeekLong(kTime, time); // ROM at 0: no system yet
	if (system) {
		const s64 behind = (s64)chip - (s64)time;
		system = behind <= 86400 && behind >= -60;
	}
	const bool fixSystem = system && !Near(time, macSeconds);
	if (Near(chip, macSeconds) && !fixSystem)
		return ClockSync::Unchanged;
	rtc.SetSeconds(macSeconds);
	if (fixSystem && cpu::PokeLong(kTime, macSeconds))
		return ClockSync::ChipAndSystem;
	return ClockSync::Chip;
}

} // namespace pimac
