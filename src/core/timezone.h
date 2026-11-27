// Network time for the Mac's clock: time zones with their daylight saving
// rules (Macs keep local time, and System 7 never changes for summer time
// by itself).
#pragma once

#include "core/compat.h"

namespace pimac {

enum TimeZone {
	TimeZoneOff, // no network time: the clock carries on from the saved time
	TimeZoneUK,  // GMT, BST in summer (EU rule)
	TimeZoneCentralEurope, // CET/CEST
	TimeZoneEasternEurope, // EET/EEST
	TimeZoneUSEastern,     // US rule: 2nd Sunday of March to 1st Sunday of November
	TimeZoneUSCentral,
	TimeZoneUSMountain,
	TimeZoneUSPacific,
	TimeZoneUTC,
	TimeZoneCount
};

const char *TimeZoneName(unsigned zone);     // for the menu: "UK", "US Pacific"...
const char *TimeZoneConfigId(unsigned zone); // pimac.cfg: "uk", "us_pacific"...
int TimeZoneFromConfigId(const char *id);    // -1 if unknown

// Unix time (UTC, seconds since 1970) -> Mac time (local, seconds since
// 1904). Unix times up to 2106 fit; Mac times run out in 2040.
u32 MacLocalTime(u32 unixUtc, unsigned zone);
// Whether summer time is in force at that moment (for the log).
bool TimeZoneSummer(u32 unixUtc, unsigned zone);

} // namespace pimac
