#include "core/log.h"

namespace pimac {

namespace {
LogHandler g_Handler = 0;
unsigned g_HexBudget = 400; // stop flooding after this many LogHex lines
} // namespace

void SetLogHandler(LogHandler handler)
{
	g_Handler = handler;
}

void LogLine(const char *line)
{
	if (g_Handler)
		g_Handler(line);
}

void LogHex(const char *tag, u32 a, u32 b)
{
	if (!g_Handler || g_HexBudget == 0)
		return;
	if (--g_HexBudget == 0) {
		g_Handler("(further I/O log messages suppressed)");
		return;
	}
	char line[96];
	StrCopy(line, tag, sizeof line);
	StrAppend(line, " ", sizeof line);
	StrAppendHex(line, a, 6, sizeof line);
	StrAppend(line, " ", sizeof line);
	StrAppendHex(line, b, 8, sizeof line);
	g_Handler(line);
}

} // namespace pimac
