// Mac clock helpers. Mac time counts seconds since 1 Jan 1904 (local time).
#pragma once

#include "core/compat.h"

namespace pimac {

// Seconds since 1904 for a calendar date/time (no time zone handling).
u32 MacSecondsFromDate(unsigned year, unsigned month, unsigned day,
		       unsigned hour, unsigned minute, unsigned second);

// Parses the compiler's __DATE__ ("Nov  3 2026") and __TIME__ ("14:05:09").
// Returns 0 if they cannot be parsed.
u32 MacSecondsFromBuildStamp(const char *date, const char *time);

} // namespace pimac
