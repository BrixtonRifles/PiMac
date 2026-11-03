#include "core/compat.h"

namespace pimac {

void StrCopy(char *dst, const char *src, unsigned cap)
{
	if (cap == 0)
		return;
	unsigned i = 0;
	if (src)
		for (; i + 1 < cap && src[i]; ++i)
			dst[i] = src[i];
	dst[i] = '\0';
}

void StrAppend(char *dst, const char *src, unsigned cap)
{
	unsigned n = 0;
	while (n < cap && dst[n])
		++n;
	if (n >= cap)
		return;
	StrCopy(dst + n, src, cap - n);
}

void StrAppendUInt(char *dst, unsigned value, unsigned cap)
{
	char tmp[12];
	unsigned i = sizeof tmp;
	tmp[--i] = '\0';
	do {
		tmp[--i] = (char)('0' + value % 10);
		value /= 10;
	} while (value && i);
	StrAppend(dst, tmp + i, cap);
}

void StrAppendHex(char *dst, u32 value, unsigned digits, unsigned cap)
{
	char tmp[9];
	if (digits > 8)
		digits = 8;
	for (unsigned i = 0; i < digits; ++i)
		tmp[i] = "0123456789ABCDEF"[(value >> ((digits - 1 - i) * 4)) & 0xF];
	tmp[digits] = '\0';
	StrAppend(dst, tmp, cap);
}

bool StrEqual(const char *a, const char *b)
{
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

static char Lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

bool StrEndsWithNoCase(const char *s, const char *suffix)
{
	unsigned n = (unsigned)strlen(s), m = (unsigned)strlen(suffix);
	if (m > n)
		return false;
	s += n - m;
	for (unsigned i = 0; i < m; ++i)
		if (Lower(s[i]) != Lower(suffix[i]))
			return false;
	return true;
}

} // namespace pimac
