// Minimal logging hook for portable code. The host (Pi kernel or test
// harness) installs a handler; without one, messages are dropped.
#pragma once

#include "core/compat.h"

namespace pimac {

typedef void (*LogHandler)(const char *line);

void SetLogHandler(LogHandler handler);
void LogLine(const char *line);
// "<tag> <hex a> <hex b>" - handy for unexpected I/O. Rate-limited.
void LogHex(const char *tag, u32 a, u32 b);

} // namespace pimac
