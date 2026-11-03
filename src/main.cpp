#include "kernel.h"

#include <circle/startup.h>

int main(void)
{
	CKernel Kernel;
	if (!Kernel.Initialize()) {
		halt();
		return EXIT_HALT;
	}

	switch (Kernel.Run()) {
	case ShutdownReboot:
		reboot();
		return EXIT_REBOOT;
	case ShutdownHalt:
	default:
		halt();
		return EXIT_HALT;
	}
}
