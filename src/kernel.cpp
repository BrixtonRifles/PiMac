#include "kernel.h"

#include "core/keymap.h"
#include "core/rom.h"
#include "core/vhd.h"
#include "host/pi_storage.h"

using namespace pimac;

static const char FromKernel[] = "kernel";

static const char kSettingsPath[] = PIMAC_DRIVE "/pimac.cfg";
static const char kRomDir[] = PIMAC_DRIVE "/roms";
static const char kDiskDir[] = PIMAC_DRIVE "/disks";
static const char *const kDiskExtensions[] = {".vhd", ".dsk", ".img", ".hda", ".hfv", 0};

static const unsigned kMaxRomSize = 1024 * 1024;
static const unsigned kFrameMicros = 16626; // compact Mac video: 60.15 Hz

CKernel::CKernel()
	: m_Timer(&m_Interrupt),
	  m_Logger(m_Options.GetLogLevel(), &m_Timer),
	  m_USBHCI(&m_Interrupt, &m_Timer, TRUE), // TRUE: plug-and-play, non-blocking
	  m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED),
	  m_bSDMounted(false),
	  m_Input(&m_DeviceNameService),
	  m_pRoms(0), m_pDisks(0), m_pMenu(0), m_pScaler(0), m_pMachine(0),
	  m_pRomData(0), m_nRomSize(0),
	  m_bMenuOpen(false), m_bMenuDirty(false)
{
	SettingsDefaults(m_Settings);
}

CKernel::~CKernel()
{
}

boolean CKernel::Initialize()
{
	boolean bOK = TRUE;

	if (bOK)
		bOK = m_Serial.Initialize(115200);
	if (bOK)
		bOK = m_Logger.Initialize(&m_Serial);
	if (bOK)
		bOK = m_Interrupt.Initialize();
	if (bOK)
		bOK = m_Timer.Initialize();

	// Bring the screen up before anything slow.
	if (bOK) {
		unsigned nWidth = m_Options.GetWidth(), nHeight = m_Options.GetHeight();
		bOK = m_Display.Initialize(nWidth ? nWidth : 1024, nHeight ? nHeight : 768);
	}

	if (bOK)
		bOK = m_USBHCI.Initialize();

	if (bOK) {
		// A missing or unreadable card is not fatal: the settings menu still works.
		if (m_EMMC.Initialize())
			m_bSDMounted = f_mount(&m_FileSystem, PIMAC_DRIVE, 1) == FR_OK;
		if (!m_bSDMounted)
			m_Logger.Write(FromKernel, LogWarning, "SD card not mounted");
	}

	if (bOK) {
		m_pRoms = new FileList;
		m_pDisks = new FileList;
		FileListClear(*m_pRoms);
		FileListClear(*m_pDisks);
		m_pMenu = new SettingsMenu;
		m_pScaler = new MonoScaler;
		m_pMachine = new TestMachine;
	}

	return bOK;
}

TShutdownMode CKernel::Run()
{
	m_Logger.Write(FromKernel, LogNotice, "PiMac starting");

	LoadSettings();
	ScanMedia();
	ProbeMedia();

	m_pScaler->Configure(m_Display.GetSurface(), TestMachine::kWidth, TestMachine::kHeight);

	unsigned nNextFrame = CTimer::GetClockTicks();
	for (;;) {
		if (m_USBHCI.UpdatePlugAndPlay())
			m_Input.UpdateDevices();

		InputEvent Event;
		while (m_Input.Queue().Pop(Event)) {
			TShutdownMode Mode = HandleEvent(Event);
			if (Mode != ShutdownNone)
				return Mode;
		}

		if (m_bMenuOpen) {
			if (m_bMenuDirty) {
				m_pMenu->Render(m_Display.GetSurface());
				m_bMenuDirty = false;
			}
		} else {
			m_pMachine->RunFrame();
			m_pScaler->Present(m_pMachine->Screen());
		}

		// Frame pacing. If we fall far behind (e.g. a slow SD access), resync
		// rather than trying to catch up.
		nNextFrame += kFrameMicros;
		int nAhead = (int)(nNextFrame - CTimer::GetClockTicks());
		if (nAhead < -100000)
			nNextFrame = CTimer::GetClockTicks();
		else
			while ((int)(nNextFrame - CTimer::GetClockTicks()) > 0) {
			}
	}
}

TShutdownMode CKernel::HandleEvent(const InputEvent &rEvent)
{
	if (rEvent.type == EvMouse) {
		if (!m_bMenuOpen) {
			m_pMachine->MouseMotion(rEvent.dx, rEvent.dy);
			m_pMachine->MouseButton(rEvent.buttons & 1);
		}
		return ShutdownNone;
	}

	const bool bDown = rEvent.type == EvKeyDown;

	if (!m_bMenuOpen) {
		if (rEvent.code == hid::F12) {
			if (bDown)
				OpenMenu();
			return ShutdownNone; // the Mac never sees F12
		}
		u8 nMacKey = MacKeyFromHID(rEvent.code, m_Settings.altIsCommand);
		if (nMacKey != kMacKeyNone)
			m_pMachine->KeyEvent(nMacKey, bDown);
		return ShutdownNone;
	}

	if (!bDown)
		return ShutdownNone;

	switch (m_pMenu->HandleKey(rEvent.code)) {
	case MenuResult::None:
		m_bMenuDirty = true;
		break;
	case MenuResult::Close:
		CloseMenu();
		break;
	case MenuResult::ApplyAndRestart:
		m_Settings = m_pMenu->Edited();
		if (!SaveSettings())
			m_Logger.Write(FromKernel, LogWarning, "Could not save settings");
		ProbeMedia();
		m_pMachine->Reset();
		CloseMenu();
		break;
	case MenuResult::RebootPi:
		return ShutdownReboot;
	}
	return ShutdownNone;
}

void CKernel::OpenMenu()
{
	m_pMachine->ReleaseAllInputs();
	ScanMedia(); // pick up files added since boot (when hot-swappable storage arrives)
	m_pMenu->Open(m_Settings, m_pRoms, m_pDisks);
	m_bMenuOpen = true;
	m_bMenuDirty = true;
}

void CKernel::CloseMenu()
{
	m_bMenuOpen = false;
	m_pScaler->Invalidate(); // repaint everything the overlay covered
}

void CKernel::LoadSettings()
{
	SettingsDefaults(m_Settings);
	if (!m_bSDMounted)
		return;

	u8 *pText = 0;
	unsigned nSize = 0;
	if (PiStorage::ReadWholeFile(kSettingsPath, &pText, &nSize, 16 * 1024)) {
		SettingsParse(m_Settings, (const char *)pText, nSize);
		delete[] pText;
		m_Logger.Write(FromKernel, LogNotice, "Settings loaded");
	}
}

bool CKernel::SaveSettings()
{
	if (!m_bSDMounted)
		return false;
	char Buffer[512];
	unsigned nSize = SettingsSerialize(m_Settings, Buffer, sizeof Buffer);
	return PiStorage::WriteWholeFile(kSettingsPath, Buffer, nSize);
}

void CKernel::ScanMedia()
{
	if (!m_bSDMounted)
		return;
	PiStorage::ListDirectory(kRomDir, *m_pRoms, 0);
	PiStorage::ListDirectory(kDiskDir, *m_pDisks, kDiskExtensions);
}

void CKernel::ProbeMedia()
{
	char Line[TestMachine::kLineLen];
	char Path[160];

	// ROM
	delete[] m_pRomData;
	m_pRomData = 0;
	m_nRomSize = 0;

	const char *pRomName = m_Settings.rom[0] ? m_Settings.rom
				: (m_pRoms->count ? m_pRoms->names[0] : 0);
	StrCopy(Line, "ROM:  ", sizeof Line);
	if (!pRomName) {
		StrAppend(Line, "none found in /roms", sizeof Line);
	} else {
		StrCopy(Path, kRomDir, sizeof Path);
		StrAppend(Path, "/", sizeof Path);
		StrAppend(Path, pRomName, sizeof Path);
		StrAppend(Line, pRomName, sizeof Line);
		if (PiStorage::ReadWholeFile(Path, &m_pRomData, &m_nRomSize, kMaxRomSize)) {
			RomInfo Info = IdentifyRom(m_pRomData, m_nRomSize);
			StrAppend(Line, " - ", sizeof Line);
			StrAppend(Line, Info.model, sizeof Line);
			m_Logger.Write(FromKernel, LogNotice, "ROM %s: %s (%08X, %u bytes)", pRomName,
				       Info.model, Info.checksum, m_nRomSize);
		} else {
			StrAppend(Line, " - unreadable", sizeof Line);
		}
	}
	m_pMachine->SetInfoLine(0, Line);

	// Hard disk
	StrCopy(Line, "Disk: ", sizeof Line);
	if (!m_Settings.disk[0]) {
		StrAppend(Line, "none selected", sizeof Line);
	} else {
		StrCopy(Path, kDiskDir, sizeof Path);
		StrAppend(Path, "/", sizeof Path);
		StrAppend(Path, m_Settings.disk, sizeof Path);
		StrAppend(Line, m_Settings.disk, sizeof Line);

		u8 Footer[kVhdFooterSize];
		u64 nFileSize = 0;
		if (!PiStorage::ReadFileTail(Path, Footer, sizeof Footer, &nFileSize)) {
			StrAppend(Line, " - unreadable", sizeof Line);
		} else {
			VhdInfo Vhd = ParseVhdFooter(Footer);
			u64 nDiskSize = nFileSize;
			if (!Vhd.valid) {
				StrAppend(Line, " - raw, ", sizeof Line);
			} else if (Vhd.diskType == VhdFixed) {
				StrAppend(Line, " - fixed VHD, ", sizeof Line);
				nDiskSize = Vhd.currentSize;
			} else {
				StrAppend(Line, " - dynamic VHD (unsupported), ", sizeof Line);
				nDiskSize = Vhd.currentSize;
			}
			StrAppendUInt(Line, (unsigned)(nDiskSize >> 20), sizeof Line);
			StrAppend(Line, " MB", sizeof Line);
		}
	}
	m_pMachine->SetInfoLine(1, Line);

	StrCopy(Line, "Model: ", sizeof Line);
	StrAppend(Line, MachineName(m_Settings.machine), sizeof Line);
	StrAppend(Line, ", ", sizeof Line);
	StrAppendUInt(Line, m_Settings.ramKB / 1024, sizeof Line);
	if (m_Settings.ramKB % 1024)
		StrAppend(Line, ".5", sizeof Line);
	StrAppend(Line, " MB RAM", sizeof Line);
	m_pMachine->SetInfoLine(2, Line);
}
