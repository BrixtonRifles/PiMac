// The few C library functions Musashi references. Circle has no C library.
// They are only reached from Musashi's fatal-error path and its 68881 FPU
// code (which a 68000 never runs), so these only need to report and halt.

#include <circle/logger.h>
#include <circle/stdarg.h>

static const char FromMusashi[] = "m68k";

extern "C" {

// newlib's `stderr` expands to _impure_ptr->_stderr. A zeroed block makes
// that a null FILE*, which the stubs below ignore.
static unsigned long s_FakeReent[256];
void *_impure_ptr = s_FakeReent;

int vfprintf(void *, const char *pFormat, va_list Args)
{
	CLogger::Get()->WriteV(FromMusashi, LogError, pFormat, Args);
	return 0;
}

int fprintf(void *pStream, const char *pFormat, ...)
{
	va_list Args;
	va_start(Args, pFormat);
	vfprintf(pStream, pFormat, Args);
	va_end(Args);
	return 0;
}

int sprintf(char *pBuffer, const char *, ...)
{
	pBuffer[0] = '\0';
	return 0;
}

int sscanf(const char *, const char *, ...)
{
	return 0;
}

void exit(int)
{
	CLogger::Get()->Write(FromMusashi, LogPanic, "fatal CPU emulation error");
	for (;;) {
	}
}

} // extern "C"
