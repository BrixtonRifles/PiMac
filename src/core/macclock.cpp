#include "core/macclock.h"

namespace pimac {

static bool IsLeap(unsigned y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

u32 MacSecondsFromDate(unsigned year, unsigned month, unsigned day,
		       unsigned hour, unsigned minute, unsigned second)
{
	static const unsigned kDaysBefore[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	if (year < 1904 || month < 1 || month > 12 || day < 1)
		return 0;
	u32 days = 0;
	for (unsigned y = 1904; y < year; ++y)
		days += IsLeap(y) ? 366 : 365;
	days += kDaysBefore[month - 1] + (month > 2 && IsLeap(year) ? 1 : 0) + (day - 1);
	return days * 86400u + hour * 3600u + minute * 60u + second;
}

static unsigned Digits(const char *s, unsigned n)
{
	unsigned v = 0;
	for (unsigned i = 0; i < n; ++i) {
		char c = s[i] == ' ' ? '0' : s[i];
		if (c < '0' || c > '9')
			return 0xFFFFFFFF;
		v = v * 10 + (unsigned)(c - '0');
	}
	return v;
}

u32 MacSecondsFromBuildStamp(const char *date, const char *time)
{
	static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	if (!date || !time || strlen(date) != 11 || strlen(time) != 8)
		return 0;
	unsigned month = 0;
	for (unsigned m = 0; m < 12; ++m)
		if (memcmp(date, kMonths + 3 * m, 3) == 0)
			month = m + 1;
	unsigned day = Digits(date + 4, 2), year = Digits(date + 7, 4);
	unsigned hh = Digits(time, 2), mm = Digits(time + 3, 2), ss = Digits(time + 6, 2);
	if (!month || day > 31 || year == 0xFFFFFFFF || hh > 23 || mm > 59 || ss > 59)
		return 0;
	return MacSecondsFromDate(year, month, day, hh, mm, ss);
}

} // namespace pimac
