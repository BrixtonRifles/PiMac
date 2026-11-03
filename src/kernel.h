#pragma once

#include <circle/actled.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>

#include "core/floppy_image.h"
#include "core/settings.h"
#include "core/settings_menu.h"
#include "core/test_machine.h"
#include "core/text_page.h"
#include "core/video.h"
#include "mac/mac_compact.h"
#include "host/pi_disk.h"
#include "host/pi_display.h"
#include "host/pi_input.h"
#include "host/pi_sound.h"

enum TShutdownMode { ShutdownNone, ShutdownHalt, ShutdownReboot };

class CKernel {
public:
	CKernel();
	~CKernel();

	boolean Initialize();
	TShutdownMode Run();

private:
	void LoadSettings();
	bool SaveSettings();
	void ScanMedia();
	void ProbeMedia();
	void StartMachine();
	void LoadPram();
	void SavePram();
	void EverySecond();
	// Per drive (0 internal, 1 external):
	bool InsertFloppy(unsigned nDrive); // m_Settings.floppy[n] in; false + m_FloppyError on failure
	void EjectFloppy(unsigned nDrive);  // host-side: save, then empty the drive
	void EjectFloppies();               // both
	void SaveFloppy(unsigned nDrive);   // write the image back to the card if the Mac changed it
	void CheckFloppies();               // per frame: notice the Mac ejecting a disk
	TShutdownMode HandleEvent(const pimac::InputEvent &rEvent);
	void OpenMenu();
	void CloseMenu();
	void BuildDiagnostics();
	void UpdateUSBStatusLine();

	// Circle system services (construction order matters)
	CActLED m_ActLED;
	CKernelOptions m_Options;
	CDeviceNameService m_DeviceNameService;
	CSerialDevice m_Serial;
	CExceptionHandler m_ExceptionHandler;
	CInterruptSystem m_Interrupt;
	CTimer m_Timer;
	CLogger m_Logger;
	CUSBHCIDevice m_USBHCI;
	CEMMCDevice m_EMMC;
	FATFS m_FileSystem;
	bool m_bSDMounted;

	// PiMac. Large objects live on the heap: the kernel object is on the
	// (small) boot stack.
	PiDisplay m_Display;
	PiInput m_Input;
	pimac::Settings m_Settings;
	pimac::FileList *m_pRoms;
	pimac::FileList *m_pDisks;
	pimac::FileList *m_pFloppies;
	// By drive: internal, external.
	pimac::FloppyImage *m_pFloppy[pimac::kFloppyDrives]; // the disk in the drive
	char m_FloppyName[pimac::kFloppyDrives][64]; // its file in /floppies (saves go here)
	unsigned m_nFloppyWritesSeen[pimac::kFloppyDrives];
	unsigned m_nFloppyQuietSeconds[pimac::kFloppyDrives];
	char m_FloppyError[pimac::kFloppyDrives][64]; // why the last insert/save failed ("" = ok)
	pimac::SettingsMenu *m_pMenu;
	pimac::MonoScaler *m_pScaler;
	pimac::TestMachine *m_pMachine;   // test screen / fallback
	pimac::MacCompact *m_pMac;        // the emulated Mac
	pimac::Machine *m_pActive;        // whichever of the two is running
	PiDiskImage *m_pDiskImage;
	PiSound *m_pSound;
	s16 *m_pSoundFrame;               // one emulated frame of samples
	unsigned m_nFrameMicros;          // average time spent emulating a frame
	unsigned m_nSecondsSincePramSave;
	u8 *m_pRomData;
	unsigned m_nRomSize;
	pimac::TextPage *m_pDiagnostics;
	char *m_pTextBuffer;
	bool m_bMenuOpen;
	bool m_bMenuDirty;
	bool m_bDiagnosticsOpen;
	unsigned m_nLastStatusTicks;
};
