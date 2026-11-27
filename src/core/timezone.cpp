#include "core/timezone.h"

namespace pimac {

namespace {

enum Rule { RuleNone, RuleEU, RuleUS };

struct Zone {
	const char *name;
	const char *id;
	int standardMinutes; // standard time minus UTC
	Rule rule;
};

const Zone kZones[TimeZoneCount] = {
	{"Off (saved time)", "off", 0, RuleNone},
	{"UK", "uk", 0, RuleEU},
	{"Central Europe", "central_europe", 60, RuleEU},
	{"Eastern Europe", "eastern_europe", 120, RuleEU},
	{"US Eastern", "us_eastern", -300, RuleUS},
	{"US Central", "us_central", -360, RuleUS},
	{"US Mountain", "us_mountain", -420, RuleUS},
	{"US Pacific", "us_pacific", -480, RuleUS},
	{"UTC", "utc", 0, RuleNone},
};

// Seconds between 1904-01-01 and 1970-01-01.
const u32 kMacToUnix = 2082844800u;

bool IsLeap(unsigned y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

// Days since 1970-01-01 of a date.
u32 DaysFromDate(unsigned year, unsigned month, unsigned day)
{
	static const unsigned kDaysBefore[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	u32 days = 0;
	for (unsigned y = 1970; y < year; ++y)
		days += IsLeap(y) ? 366 : 365;
	return days + kDaysBefore[month - 1] + (month > 2 && IsLeap(year) ? 1 : 0) + (day - 1);
}

unsigned YearOf(u32 unixTime)
{
	u32 days = unixTime / 86400;
	unsigned year = 1970;
	for (;;) {
		const u32 n = IsLeap(year) ? 366 : 365;
		if (days < n)
			return year;
		days -= n;
		++year;
	}
}

// 1970-01-01 was a Thursday; 0 = Sunday.
unsigned Weekday(u32 days)
{
	return (days + 4) % 7;
}

// Days since 1970 of the nth Sunday of a month (n = 0: the last one).
u32 Sunday(unsigned year, unsigned month, unsigned n)
{
	if (n == 0) {
		static const unsigned kLength[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
		const unsigned last = kLength[month - 1] + (month == 2 && IsLeap(year) ? 1 : 0);
		const u32 d = DaysFromDate(year, month, last);
		return d - Weekday(d);
	}
	const u32 first = DaysFromDate(year, month, 1);
	return first + (7 - Weekday(first)) % 7 + 7 * (n - 1);
}

bool Summer(u32 utc, const Zone &z)
{
	const unsigned year = YearOf(utc);
	u32 start, end; // UTC
	if (z.rule == RuleEU) {
		// Last Sunday of March to last Sunday of October, 01:00 UTC everywhere.
		start = Sunday(year, 3, 0) * 86400 + 3600;
		end = Sunday(year, 10, 0) * 86400 + 3600;
	} else if (z.rule == RuleUS) {
		// 02:00 local standard time on the 2nd Sunday of March, to 02:00
		// local summer time (01:00 standard) on the 1st Sunday of November.
		const int std = z.standardMinutes * 60;
		start = (u32)((s64)Sunday(year, 3, 2) * 86400 + 7200 - std);
		end = (u32)((s64)Sunday(year, 11, 1) * 86400 + 3600 - std);
	} else {
		return false;
	}
	return utc >= start && utc < end;
}

} // namespace

const char *TimeZoneName(unsigned zone)
{
	return zone < TimeZoneCount ? kZones[zone].name : "?";
}

const char *TimeZoneConfigId(unsigned zone)
{
	return zone < TimeZoneCount ? kZones[zone].id : "off";
}

int TimeZoneFromConfigId(const char *id)
{
	for (unsigned z = 0; z < TimeZoneCount; ++z)
		if (StrEqual(kZones[z].id, id))
			return (int)z;
	return -1;
}

bool TimeZoneSummer(u32 unixUtc, unsigned zone)
{
	return zone < TimeZoneCount && Summer(unixUtc, kZones[zone]);
}

u32 MacLocalTime(u32 unixUtc, unsigned zone)
{
	const Zone &z = kZones[zone < TimeZoneCount ? zone : TimeZoneUTC];
	const s64 offset = (s64)z.standardMinutes * 60 + (Summer(unixUtc, z) ? 3600 : 0);
	return (u32)((s64)unixUtc + kMacToUnix + offset);
}

} // namespace pimac
