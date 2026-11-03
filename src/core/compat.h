// Portability layer so everything under core/ builds both bare-metal (Circle)
// and natively on the development machine (unit tests, previews).
// core/ must not include any Circle header other than through this file.
#pragma once

#ifdef PIMAC_HOST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#else
#include <circle/types.h>
#include <circle/util.h>
#endif

namespace pimac {

// Bounded string helpers; always NUL-terminate when cap > 0.
void StrCopy(char *dst, const char *src, unsigned cap);
void StrAppend(char *dst, const char *src, unsigned cap);
void StrAppendUInt(char *dst, unsigned value, unsigned cap);
void StrAppendHex(char *dst, u32 value, unsigned digits, unsigned cap);
bool StrEqual(const char *a, const char *b);
bool StrEndsWithNoCase(const char *s, const char *suffix);

} // namespace pimac
