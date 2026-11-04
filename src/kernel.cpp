#include "kernel.h"

#include "core/keymap.h"
#include "core/log.h"
#include "core/macclock.h"
#include "core/rom.h"
#include "core/vhd.h"
#include "host/pi_storage.h"

#include <circle/bcmpropertytags.h>
#include <circle/string.h>

using namespace pimac;

static const char FromKernel[] = "kernel";

static const unsigned kTextBufferSize = LOGGER_BUFSIZE + 1;

// A CDevice that captures whatever is written to it (for ListDevices()).
class CTextCapture : public CDevice {
public:
	CTextCapture(char *pBuffer, unsigned nSize) : m_pBuffer(pBuffer), m_nSize(nSize), m_nUsed(0) {}

	int Write(const void *pBuffer, size_t nCount) override
	{
		const char *p = (const char *)pBuffer;
		for (size_t i = 0; i < nCount && m_nUsed + 1 < m_nSize; ++i)
			m_pBuffer[m_nUsed++] = p[i];
		return (int)nCount;
	}

	unsigned GetLength() const { return m_nUsed; }

private:
	char *m_pBuffer;
	unsigned m_nSize;
	unsigned m_nUsed;
};

static const char kSettingsPath[] = PIMAC_DRIVE "/pimac.cfg";
// 256 bytes PRAM + clock, one file per model (their PRAM layouts differ).
static const char kPramPath[] = PIMAC_DRIVE "/pimac.pram";
static const char kPramPathClassic2[] = PIMAC_DRIVE "/pimac-classic2.pram";
static const char kPramPathColorClassic[] = PIMAC_DRIVE "/pimac-colorclassic.pram";
static const char kRomDir[] = PIMAC_DRIVE "/roms";
static const char kDiskDir[] = PIMAC_DRIVE "/disks";
static const char *const kDiskExtensions[] = {".vhd", ".dsk", ".img", ".hda", ".hfv", 0};
static const char kFloppyDir[] = PIMAC_DRIVE "/floppies";
static const char *const kFloppyExtensions[] = {".dsk", ".img", ".image", ".dc42", ".diskcopy", 0};
static const unsigned kMaxFloppySize = 2 * 1024 * 1024;
static const char *const kFloppyDriveName[] = {"Floppy", "External floppy"};
static const unsigned kFloppySaveDelay = 2; // seconds without Mac writes before saving
static const char kCdromDir[] = PIMAC_DRIVE "/cdroms"; // the drive is SCSI ID 3
static const char *const kCdromExtensions[] = {".toast", ".iso", ".cdr", ".img", 0};

static const unsigned kMaxRomSize = 1024 * 1024;
static const unsigned kFrameMicros = 16626; // compact Mac video: 60.15 Hz
static const unsigned kSoundFrameMax = 512;   // samples per emulated frame (Mac: 370)

static void LogToCircle(const char *pLine)
{
	CLogger::Get()->Write("mac", LogNotice, "%s", pLine);
}

CKernel::CKernel()
	: m_Timer(&m_Interrupt),
	  m_Logger(m_Options.GetLogLevel(), &m_Timer),
	  m_CPUThrottle(CPUSpeedMaximum),
	  m_USBHCI(&m_Interrupt, &m_Timer, TRUE), // TRUE: plug-and-play, non-blocking
	  m_EMMC(&m_Interrupt, &m_Timer, &m_ActLED),
	  m_bSDMounted(false),
	  m_Input(&m_DeviceNameService),
	  m_pRoms(0), m_pDisks(0), m_pFloppies(0), m_pCdroms(0), m_pCdImage(0), m_pMenu(0), m_pScaler(0), m_pMachine(0),
	  m_pCompact(0), m_pClassic2(0), m_pColorClassic(0), m_pMac(0), m_pPramPath(0), m_pActive(0), m_pDiskImage(0), m_pSound(0), m_pSoundFrame(0), m_nFrameMicros(0), m_nSecondsSincePramSave(0),
	  m_pRomData(0), m_nRomSize(0),
	  m_pDiagnostics(0), m_pTextBuffer(0),
	  m_bMenuOpen(false), m_bMenuDirty(false), m_bDiagnosticsOpen(false),
	  m_nLastStatusTicks(0),
	  m_pNet(0), m_pLocalTalkLink(0), m_pModemLink(0), m_pLocalTalk(0), m_pModem(0)
{
	SettingsDefaults(m_Settings);
	m_CdromError[0] = '\0';
	for (unsigned nDrive = 0; nDrive < kFloppyDrives; ++nDrive) {
		m_pFloppy[nDrive] = 0;
		m_nFloppyWritesSeen[nDrive] = m_nFloppyQuietSeconds[nDrive] = 0;
		m_FloppyError[nDrive][0] = '\0';
		m_FloppyName[nDrive][0] = '\0';
	}
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
		// Not fatal: without it the serial ports just stay unconnected. The
		// USB Ethernet chip appears later (plug-and-play) and DHCP runs in
		// the background.
		m_pNet = new CNetSubSystem(0, 0, 0, 0, "pimac");
		if (!m_pNet->Initialize(FALSE)) {
			m_Logger.Write(FromKernel, LogWarning, "Network did not start");
			delete m_pNet;
			m_pNet = 0;
		}
	}

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
		m_pFloppies = new FileList;
		FileListClear(*m_pFloppies);
		m_pCdroms = new FileList;
		FileListClear(*m_pCdroms);
		m_pCdImage = new PiDiskImage;
		for (unsigned nDrive = 0; nDrive < kFloppyDrives; ++nDrive)
			m_pFloppy[nDrive] = new FloppyImage;
		m_pMenu = new SettingsMenu;
		m_pScaler = new MonoScaler;
		m_pSound = new PiSound(&m_Interrupt);
		m_pSoundFrame = new s16[kSoundFrameMax];
		m_pMachine = new TestMachine;
		m_pCompact = new MacCompact;
		m_pClassic2 = new MacV8;
		m_pColorClassic = new MacV8(MacV8::ModelColorClassic);
		m_pMac = m_pCompact;
		m_pActive = m_pMachine;
		m_pDiskImage = new PiDiskImage;
		SetLogHandler(LogToCircle);
		m_pDiagnostics = new TextPage;
		m_pTextBuffer = new char[kTextBufferSize];
		m_pLocalTalkLink = new PiMulticastLink;
		m_pModemLink = new PiStreamLink;
		m_pLocalTalk = new LocalTalkPort;
		m_pModem = new HayesModem;
	}

	return bOK;
}

TShutdownMode CKernel::Run()
{
	m_Logger.Write(FromKernel, LogNotice, "PiMac starting");

	LoadSettings();
	ScanMedia();

	// The sender ID only has to differ between emulators on one network:
	// the board serial number does.
	CBcmPropertyTags Tags;
	TPropertyTagSerial Serial;
	u32 nSenderId = CTimer::GetClockTicks();
	if (Tags.GetTag(PROPTAG_GET_BOARD_SERIAL, &Serial, sizeof Serial))
		nSenderId = Serial.Serial[0] ^ Serial.Serial[1];
	m_pLocalTalk->SetLink(m_pLocalTalkLink, nSenderId);
	if (m_pNet)
		m_pModemLink->Start(m_pNet, (u16)m_Settings.modemListen);
	m_pModem->SetLink(m_pModemLink);

	StartMachine();
	m_pSound->Start(m_Settings.sound);

	m_pScaler->Configure(m_Display.GetSurface(), TestMachine::kWidth, TestMachine::kHeight);

	unsigned nNextFrame = CTimer::GetClockTicks();
	for (;;) {
		if (m_USBHCI.UpdatePlugAndPlay())
			m_Input.UpdateDevices();

		m_pLocalTalkLink->Poll(m_pNet);
		m_pModem->Poll(CTimer::GetClockTicks() / 1000);

		InputEvent Event;
		while (m_Input.Queue().Pop(Event)) {
			TShutdownMode Mode = HandleEvent(Event);
			if (Mode != ShutdownNone)
				return Mode;
		}

		// Refresh USB status once a second (live in the diagnostics page too).
		if (CTimer::GetClockTicks() - m_nLastStatusTicks >= 1000000) {
			m_nLastStatusTicks = CTimer::GetClockTicks();
			EverySecond();
			UpdateUSBStatusLine();
			if (m_bDiagnosticsOpen) {
				BuildDiagnostics();
				m_bMenuDirty = true;
			}
		}

		if (m_bMenuOpen) {
			if (m_bMenuDirty) {
				if (m_bDiagnosticsOpen)
					m_pDiagnostics->Render(m_Display.GetSurface(), "Diagnostics");
				else
					m_pMenu->Render(m_Display.GetSurface());
				m_bMenuDirty = false;
			}
		} else {
			unsigned nStart = CTimer::GetClockTicks();
			m_pActive->RunFrame();
			u32 nRate;
			unsigned nSamples = m_pActive->SoundFrame(m_pSoundFrame, kSoundFrameMax, nRate);
			m_pSound->Feed(m_pSoundFrame, nSamples, nRate);
			CheckFloppies();
			CheckCdrom();
			unsigned nUsed = CTimer::GetClockTicks() - nStart;
			m_nFrameMicros = (m_nFrameMicros * 15 + nUsed) / 16;
			PresentScreen();
		}

		// Frame pacing. If we fall far behind (e.g. a slow SD access), resync
		// rather than trying to catch up. The network tasks run while we wait.
		nNextFrame += kFrameMicros;
		int nAhead = (int)(nNextFrame - CTimer::GetClockTicks());
		if (nAhead < -100000)
			nNextFrame = CTimer::GetClockTicks();
		else
			do
				m_Scheduler.Yield();
			while ((int)(nNextFrame - CTimer::GetClockTicks()) > 0);
	}
}

void CKernel::PresentScreen()
{
	ColorFrame Color;
	if (m_pActive->ColorScreen(Color)) {
		m_pScaler->Present(Color);
		if (m_pScaler->HostPaletteChanged())
			m_Display.SetMacPalette(m_pScaler->HostPalette());
	} else {
		m_pScaler->Present(m_pActive->Screen());
	}
}

TShutdownMode CKernel::HandleEvent(const InputEvent &rEvent)
{
	if (rEvent.type == EvMouse) {
		if (!m_bMenuOpen) {
			m_pActive->MouseMotion(rEvent.dx, rEvent.dy);
			m_pActive->MouseButton(rEvent.buttons & 1);
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
			m_pActive->KeyEvent(nMacKey, bDown);
		return ShutdownNone;
	}

	if (!bDown)
		return ShutdownNone;

	if (m_bDiagnosticsOpen) {
		if (m_pDiagnostics->HandleKey(rEvent.code)) {
			// Back to the menu: clear the larger diagnostics panel first.
			m_bDiagnosticsOpen = false;
			m_pScaler->Invalidate();
			PresentScreen();
		}
		m_bMenuDirty = true;
		return ShutdownNone;
	}

	switch (m_pMenu->HandleKey(rEvent.code)) {
	case MenuResult::None:
		m_bMenuDirty = true;
		break;
	case MenuResult::Close:
		CloseMenu();
		break;
	case MenuResult::ApplyAndRestart: {
		const unsigned nOldSound = m_Settings.sound;
		m_Settings = m_pMenu->Edited();
		if (!SaveSettings())
			m_Logger.Write(FromKernel, LogWarning, "Could not save settings");
		SavePram();
		StartMachine();
		if (m_Settings.sound != nOldSound)
			m_pSound->Start(m_Settings.sound);
		CloseMenu();
		break;
	}
	case MenuResult::ChangeFloppy: {
		const unsigned nDrive = m_pMenu->ChangedDrive();
		EjectFloppy(nDrive);
		StrCopy(m_Settings.floppy[nDrive], m_pMenu->Edited().floppy[nDrive],
			sizeof m_Settings.floppy[nDrive]);
		bool bOK = InsertFloppy(nDrive);
		if (!SaveSettings())
			m_Logger.Write(FromKernel, LogWarning, "Could not save settings");
		if (bOK) {
			CloseMenu();
		} else {
			char Msg[80];
			StrCopy(Msg, "Cannot insert: ", sizeof Msg);
			StrAppend(Msg, m_FloppyError[nDrive], sizeof Msg);
			m_pMenu->SetFloppyInDrive(nDrive, m_Settings.floppy[nDrive]);
			m_pMenu->SetMessage(Msg);
			m_bMenuDirty = true;
		}
		break;
	}
	case MenuResult::ChangeCdrom: {
		EjectCdrom();
		StrCopy(m_Settings.cdrom, m_pMenu->Edited().cdrom, sizeof m_Settings.cdrom);
		bool bOK = InsertCdrom();
		if (!SaveSettings())
			m_Logger.Write(FromKernel, LogWarning, "Could not save settings");
		if (bOK) {
			CloseMenu();
		} else {
			char Msg[80];
			StrCopy(Msg, "Cannot insert: ", sizeof Msg);
			StrAppend(Msg, m_CdromError, sizeof Msg);
			m_pMenu->SetCdromInDrive(m_Settings.cdrom);
			m_pMenu->SetMessage(Msg);
			m_bMenuDirty = true;
		}
		break;
	}
	case MenuResult::ShowDiagnostics:
		BuildDiagnostics();
		m_bDiagnosticsOpen = true;
		m_bMenuDirty = true;
		break;
	case MenuResult::RebootPi:
		EjectFloppies(); // saves them
		EjectCdrom();
		SavePram();
		return ShutdownReboot;
	}
	return ShutdownNone;
}

void CKernel::OpenMenu()
{
	m_pActive->ReleaseAllInputs();
	ScanMedia(); // pick up files added since boot (when hot-swappable storage arrives)
	m_pMenu->Open(m_Settings, m_pRoms, m_RomInfo, m_pDisks, m_pFloppies, m_pCdroms);
	m_bMenuOpen = true;
	m_bMenuDirty = true;
}

void CKernel::CloseMenu()
{
	m_bMenuOpen = false;
	m_bDiagnosticsOpen = false;
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
	char Buffer[1024];
	unsigned nSize = SettingsSerialize(m_Settings, Buffer, sizeof Buffer);
	return PiStorage::WriteWholeFile(kSettingsPath, Buffer, nSize);
}

void CKernel::ScanMedia()
{
	if (!m_bSDMounted)
		return;
	PiStorage::ListDirectory(kRomDir, *m_pRoms, 0);
	for (unsigned i = 0; i < m_pRoms->count; ++i) { // the menu shows each ROM's model
		char Path[160];
		StrCopy(Path, kRomDir, sizeof Path);
		StrAppend(Path, "/", sizeof Path);
		StrAppend(Path, m_pRoms->names[i], sizeof Path);
		u8 Head[4];
		const bool bOK = PiStorage::ReadFileHead(Path, Head, sizeof Head);
		m_RomInfo[i] = IdentifyRom(bOK ? Head : 0, bOK ? sizeof Head : 0);
	}
	PiStorage::ListDirectory(kDiskDir, *m_pDisks, kDiskExtensions);
	PiStorage::ListDirectory(kFloppyDir, *m_pFloppies, kFloppyExtensions);
	PiStorage::ListDirectory(kCdromDir, *m_pCdroms, kCdromExtensions);
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

	// The ROM decides the model, and the model which RAM sizes work.
	const RomInfo Info = IdentifyRom(m_pRomData, m_nRomSize);
	StrCopy(Line, "Model: ", sizeof Line);
	if (!m_pRomData) {
		StrAppend(Line, "none (no ROM)", sizeof Line);
	} else {
		StrAppend(Line, Info.model, sizeof Line);
		const unsigned nRamKB = FitRamKB(Info.family, m_Settings.ramKB);
		StrAppend(Line, ", ", sizeof Line);
		StrAppendUInt(Line, nRamKB / 1024, sizeof Line);
		if (nRamKB % 1024)
			StrAppend(Line, ".5", sizeof Line);
		StrAppend(Line, " MB RAM", sizeof Line);
	}
	m_pMachine->SetInfoLine(2, Line);
}

void CKernel::UpdateUSBStatusLine()
{
	char Line[TestMachine::kLineLen];
	StrCopy(Line, "USB:  keyboards ", sizeof Line);
	StrAppendUInt(Line, m_Input.KeyboardCount(), sizeof Line);
	StrAppend(Line, ", mice ", sizeof Line);
	StrAppendUInt(Line, m_Input.MouseCount(), sizeof Line);
	StrAppend(Line, " (", sizeof Line);
	StrAppendUInt(Line, m_Input.MouseReports(), sizeof Line);
	StrAppend(Line, " mouse reports)", sizeof Line);
	m_pMachine->SetInfoLine(3, Line);
}

void CKernel::BuildDiagnostics()
{
	TextPage &Page = *m_pDiagnostics;
	char Line[TextPage::kLineLen];

	Page.Clear();

	StrCopy(Line, "Keyboards in use: ", sizeof Line);
	StrAppendUInt(Line, m_Input.KeyboardCount(), sizeof Line);
	StrAppend(Line, ", reports received: ", sizeof Line);
	StrAppendUInt(Line, m_Input.KeyboardReports(), sizeof Line);
	Page.AddLine(Line);

	StrCopy(Line, "Mice in use:      ", sizeof Line);
	StrAppendUInt(Line, m_Input.MouseCount(), sizeof Line);
	StrAppend(Line, ", reports received: ", sizeof Line);
	StrAppendUInt(Line, m_Input.MouseReports(), sizeof Line);
	StrAppend(Line, "   (move the mouse - this updates live)", sizeof Line);
	Page.AddLine(Line);

	if (m_pActive == m_pMac) {
		StrCopy(Line, "Emulator: running, ", sizeof Line);
		StrAppendUInt(Line, m_nFrameMicros * 100 / kFrameMicros, sizeof Line);
		StrAppend(Line, "% of each frame used.  SCSI blocks read ", sizeof Line);
		StrAppendUInt(Line, m_pMac->GetScsi().BlocksRead(), sizeof Line);
		StrAppend(Line, ", written ", sizeof Line);
		StrAppendUInt(Line, m_pMac->GetScsi().BlocksWritten(), sizeof Line);
		if (m_pMac->GetScsi().IoError())
			StrAppend(Line, "  DISK I/O ERROR", sizeof Line);
		Page.AddLine(Line);
		for (unsigned nDrive = 0; nDrive < kFloppyDrives; ++nDrive) {
			const FloppyImage *pFloppy = m_pFloppy[nDrive];
			StrCopy(Line, kFloppyDriveName[nDrive], sizeof Line);
			StrAppend(Line, ": ", sizeof Line);
			if (pFloppy->Present()) {
				const Iwm &rIwm = m_pMac->GetIwm();
				StrAppend(Line, m_FloppyName[nDrive], sizeof Line);
				StrAppend(Line, " (", sizeof Line);
				StrAppend(Line, pFloppy->SizeText(), sizeof Line);
				StrAppend(Line, pFloppy->Writable() ? ")" : ", locked)", sizeof Line);
				StrAppend(Line, ", sectors read ", sizeof Line);
				StrAppendUInt(Line, rIwm.SectorsRead(nDrive), sizeof Line);
				StrAppend(Line, ", written ", sizeof Line);
				StrAppendUInt(Line, rIwm.SectorsWritten(nDrive), sizeof Line);
				if (rIwm.BadWrites(nDrive)) {
					StrAppend(Line, ", bad writes ", sizeof Line);
					StrAppendUInt(Line, rIwm.BadWrites(nDrive), sizeof Line);
				}
				if (pFloppy->Dirty())
					StrAppend(Line, ", not yet saved", sizeof Line);
			} else {
				StrAppend(Line, "drive empty", sizeof Line);
			}
			if (m_FloppyError[nDrive][0]) {
				StrAppend(Line, "  - ", sizeof Line);
				StrAppend(Line, m_FloppyError[nDrive], sizeof Line);
			}
			Page.AddLine(Line);
		}
		StrCopy(Line, "CD-ROM (SCSI ID 3): ", sizeof Line);
		if (m_pMac->GetScsi().CdromLoaded()) {
			StrAppend(Line, m_Settings.cdrom, sizeof Line);
			StrAppend(Line, m_pCdImage->FastSeek() ? " (fast seek)" : " (SLOW seek)", sizeof Line);
		} else {
			StrAppend(Line, "no disc", sizeof Line);
		}
		if (m_CdromError[0]) {
			StrAppend(Line, "  - ", sizeof Line);
			StrAppend(Line, m_CdromError, sizeof Line);
		}
		Page.AddLine(Line);
		StrCopy(Line, "Disk image: ", sizeof Line);
		StrAppend(Line, m_pDiskImage->IsOpen() ? (m_pDiskImage->FastSeek() ? "open, fast seek" : "open, SLOW seek") : "none", sizeof Line);
		Page.AddLine(Line);
	} else {
		Page.AddLine("Emulator: not running (see the test screen for why)");
	}

	StrCopy(Line, "CPU: ", sizeof Line);
	StrAppendUInt(Line, m_CPUThrottle.GetClockRate() / 1000000, sizeof Line);
	StrAppend(Line, " MHz (max ", sizeof Line);
	StrAppendUInt(Line, m_CPUThrottle.GetMaxClockRate() / 1000000, sizeof Line);
	StrAppend(Line, "), SoC ", sizeof Line);
	StrAppendUInt(Line, m_CPUThrottle.GetTemperature(), sizeof Line);
	StrAppend(Line, " C (slows down above ", sizeof Line);
	StrAppendUInt(Line, m_CPUThrottle.GetMaxTemperature(), sizeof Line);
	StrAppend(Line, " C)", sizeof Line);
	Page.AddLine(Line);

	StrCopy(Line, "Sound: ", sizeof Line);
	StrAppend(Line, m_pSound->DeviceName(), sizeof Line);
	if (m_pSound->Active()) {
		StrAppend(Line, ", queue ", sizeof Line);
		StrAppendUInt(Line, m_pSound->QueuedMs(), sizeof Line);
		StrAppend(Line, " ms, rate trim ", sizeof Line);
		int nPpm = m_pSound->AdjustPpm();
		StrAppend(Line, nPpm < 0 ? "-" : "+", sizeof Line);
		StrAppendUInt(Line, (unsigned)(nPpm < 0 ? -nPpm : nPpm), sizeof Line);
		StrAppend(Line, " ppm, refills ", sizeof Line);
		StrAppendUInt(Line, m_pSound->Refills(), sizeof Line);
	} else if (m_Settings.sound != SoundOff) {
		StrAppend(Line, " (failed to start - see log)", sizeof Line);
	}
	Page.AddLine(Line);

	AddNetworkLines(Page);

	Page.AddLine("");
	Page.AddLine("Registered devices:");
	CTextCapture Capture(m_pTextBuffer, kTextBufferSize);
	m_DeviceNameService.ListDevices(&Capture, TRUE);
	Page.AddText(m_pTextBuffer, Capture.GetLength());

	Page.AddLine("");
	Page.AddLine("System log:");
	int nLength = m_Logger.Read(m_pTextBuffer, kTextBufferSize - 1, FALSE);
	if (nLength > 0)
		Page.AddText(m_pTextBuffer, (unsigned)nLength);
}

void CKernel::AttachSerialPorts()
{
	m_pModem->Reset(); // hangs up
	Scc &rScc = m_pMac->GetScc();
	rScc.Attach(Scc::kChannelA, m_Settings.modem ? m_pModem : 0);
	rScc.Attach(Scc::kChannelB, m_Settings.localTalk ? m_pLocalTalk : 0);
}

void CKernel::AddNetworkLines(TextPage &rPage)
{
	char Line[TextPage::kLineLen];
	char *pLine = Line;
	const unsigned nSize = sizeof Line;
	StrCopy(pLine, "Network: ", nSize);
	if (!m_pNet) {
		StrAppend(pLine, "failed to start", nSize);
		rPage.AddLine(Line);
		return;
	}
	if (!m_pNet->IsRunning()) {
		StrAppend(pLine, "waiting for Ethernet / DHCP", nSize);
	} else {
		CString Address;
		m_pNet->GetConfig()->GetIPAddress()->Format(&Address);
		StrAppend(pLine, Address, nSize);
	}
	StrAppend(pLine, ".  Modem port: ", nSize);
	if (!m_Settings.modem)
		StrAppend(pLine, "off", nSize);
	else if (m_pModem->Online()) {
		StrAppend(pLine, "online to ", nSize);
		StrAppend(pLine, m_pModem->Remote(), nSize);
	} else
		StrAppend(pLine, "idle", nSize);
	if (m_Settings.modemListen) {
		StrAppend(pLine, ", answers port ", nSize);
		StrAppendUInt(pLine, m_Settings.modemListen, nSize);
	}
	rPage.AddLine(Line);
	StrCopy(pLine, "LocalTalk (printer port): ", nSize);
	if (!m_Settings.localTalk) {
		StrAppend(pLine, "off", nSize);
		rPage.AddLine(Line);
		return;
	}
	if (m_pLocalTalk->NodeAddress()) {
		StrAppend(pLine, "node ", nSize);
		StrAppendUInt(pLine, m_pLocalTalk->NodeAddress(), nSize);
		StrAppend(pLine, ", ", nSize);
	}
	StrAppendUInt(pLine, m_pLocalTalk->FramesSent(), nSize);
	StrAppend(pLine, " frames sent, ", nSize);
	StrAppendUInt(pLine, m_pLocalTalk->FramesReceived(), nSize);
	StrAppend(pLine, " received", nSize);
	if (!m_pLocalTalkLink->IsOpen())
		StrAppend(pLine, " (not on the network yet)", nSize);
	rPage.AddLine(Line);
}

void CKernel::StartMachine()
{
	EjectFloppies(); // saves any changes; reinserted below once the Mac is running
	EjectCdrom();
	ProbeMedia();  // loads the ROM and fills in the test screen's info lines

	m_pActive = m_pMachine;
	m_pDiskImage->Close();

	if (!m_pRomData)
		return;

	RomInfo Info = IdentifyRom(m_pRomData, m_nRomSize);
	if (Info.family == RomClassic2) {
		m_pMac = m_pClassic2;
		m_pPramPath = kPramPathClassic2;
	} else if (Info.family == RomColorClassic) {
		m_pMac = m_pColorClassic;
		m_pPramPath = kPramPathColorClassic;
	} else {
		m_pMac = m_pCompact;
		m_pPramPath = kPramPath;
	}
	if (Info.family == RomNotEmulated) {
		char Line[TestMachine::kLineLen];
		StrCopy(Line, "Model: ", sizeof Line);
		StrAppend(Line, Info.model, sizeof Line);
		StrAppend(Line, " - not emulated yet", sizeof Line);
		m_pMachine->SetInfoLine(2, Line);
		return;
	}

	if (m_Settings.disk[0]) {
		char Path[160];
		StrCopy(Path, PIMAC_DRIVE "/disks/", sizeof Path);
		StrAppend(Path, m_Settings.disk, sizeof Path);
		if (!m_pDiskImage->Open(Path))
			m_Logger.Write(FromKernel, LogWarning, "Cannot open disk image %s", Path);
	}

	MacConfig Config;
	Config.ramKB = FitRamKB(Info.family, m_Settings.ramKB);
	Config.rom = m_pRomData;
	Config.romSize = m_nRomSize;
	Config.disk = m_pDiskImage->IsOpen() ? m_pDiskImage : 0;
	Config.clockSeconds = MacSecondsFromBuildStamp(__DATE__, __TIME__);

	if (!m_pMac->Start(Config)) {
		char Line[TestMachine::kLineLen];
		StrCopy(Line, "Model: cannot start - ", sizeof Line);
		StrAppend(Line, m_pMac->Error(), sizeof Line);
		m_pMachine->SetInfoLine(2, Line);
		return;
	}

	LoadPram(); // restores PRAM and the clock saved last time, if any
	m_pActive = m_pMac;
	AttachSerialPorts();
	for (unsigned nDrive = 0; nDrive < kFloppyDrives; ++nDrive)
		InsertFloppy(nDrive); // before the first frame, so the Mac can boot from it
	InsertCdrom();
	m_pScaler->Invalidate();
	m_Logger.Write(FromKernel, LogNotice, "Emulating %s, %u KB RAM", Info.model, Config.ramKB);
}

void CKernel::LoadPram()
{
	u8 *pData = 0;
	unsigned nSize = 0;
	if (!m_bSDMounted || !PiStorage::ReadWholeFile(m_pPramPath, &pData, &nSize, 1024))
		return;
	if (nSize == Rtc::kPramSize + 4) {
		Rtc &rRtc = m_pMac->GetRtc();
		memcpy(rRtc.Pram(), pData, Rtc::kPramSize);
		const u8 *p = pData + Rtc::kPramSize;
		u32 nSaved = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
		if (nSaved > rRtc.Seconds()) // never go back before the build date
			rRtc.SetSeconds(nSaved);
		rRtc.TakePramDirty();
	}
	delete[] pData;
}

void CKernel::SavePram()
{
	if (!m_bSDMounted || m_pActive != m_pMac)
		return;
	Rtc &rRtc = m_pMac->GetRtc();
	u8 Data[Rtc::kPramSize + 4];
	memcpy(Data, rRtc.Pram(), Rtc::kPramSize);
	u32 nSeconds = rRtc.Seconds();
	Data[Rtc::kPramSize + 0] = (u8)(nSeconds >> 24);
	Data[Rtc::kPramSize + 1] = (u8)(nSeconds >> 16);
	Data[Rtc::kPramSize + 2] = (u8)(nSeconds >> 8);
	Data[Rtc::kPramSize + 3] = (u8)nSeconds;
	if (PiStorage::WriteWholeFile(m_pPramPath, Data, sizeof Data))
		rRtc.TakePramDirty();
	m_nSecondsSincePramSave = 0;
}

void CKernel::EverySecond()
{
	m_pDiskImage->Sync(); // push the Mac's disk writes out to the card
	m_CPUThrottle.Update(); // full speed unless the SoC is too hot


	if (m_pActive != m_pMac)
		return;
	// Save PRAM soon after it changes, and the clock every few minutes (the
	// Pi has no battery-backed clock, so this is what survives power-off).
	++m_nSecondsSincePramSave;
	Rtc &rRtc = m_pMac->GetRtc();
	if ((rRtc.PramDirty() && m_nSecondsSincePramSave >= 5) || m_nSecondsSincePramSave >= 300)
		SavePram();

	// Write each floppy back once the Mac has stopped writing to it for a
	// moment (a file copy is many separate writes).
	for (unsigned nDrive = 0; nDrive < kFloppyDrives; ++nDrive) {
		const unsigned nWrites = m_pMac->GetIwm().SectorsWritten(nDrive);
		if (nWrites != m_nFloppyWritesSeen[nDrive]) {
			m_nFloppyWritesSeen[nDrive] = nWrites;
			m_nFloppyQuietSeconds[nDrive] = 0;
		} else if (m_pFloppy[nDrive]->Dirty() &&
			   ++m_nFloppyQuietSeconds[nDrive] >= kFloppySaveDelay) {
			SaveFloppy(nDrive);
		}
	}
}

bool CKernel::InsertFloppy(unsigned nDrive)
{
	char *pName = m_Settings.floppy[nDrive];
	char *pError = m_FloppyError[nDrive];
	FloppyImage *pFloppy = m_pFloppy[nDrive];
	pError[0] = '\0';
	if (!pName[0] || m_pActive != m_pMac)
		return true;

	char Path[160];
	StrCopy(Path, kFloppyDir, sizeof Path);
	StrAppend(Path, "/", sizeof Path);
	StrAppend(Path, pName, sizeof Path);

	u8 *pData = 0;
	unsigned nSize = 0;
	if (m_pFloppy[1 - nDrive]->Present() && StrEqual(pName, m_FloppyName[1 - nDrive])) {
		StrCopy(pError, "already in the other drive", sizeof m_FloppyError[nDrive]);
	} else if (!PiStorage::ReadWholeFile(Path, &pData, &nSize, kMaxFloppySize)) {
		StrCopy(pError, "cannot read the file", sizeof m_FloppyError[nDrive]);
	} else {
		FloppyImage::Result Result =
			pFloppy->Attach(pData, nSize, !PiStorage::IsReadOnly(Path)); // frees pData on failure
		if (Result != FloppyImage::Ok)
			StrCopy(pError, FloppyImage::ResultText(Result), sizeof m_FloppyError[nDrive]);
	}
	if (pError[0]) {
		m_Logger.Write(FromKernel, LogWarning, "%s %s: %s", kFloppyDriveName[nDrive], pName, pError);
		pName[0] = '\0';
		return false;
	}

	StrCopy(m_FloppyName[nDrive], pName, sizeof m_FloppyName[nDrive]);
	m_pMac->GetIwm().Insert(nDrive, pFloppy);
	m_nFloppyWritesSeen[nDrive] = m_pMac->GetIwm().SectorsWritten(nDrive);
	m_nFloppyQuietSeconds[nDrive] = 0;
	m_Logger.Write(FromKernel, LogNotice, "%s %s inserted (%s%s)", kFloppyDriveName[nDrive], pName,
		       pFloppy->SizeText(), pFloppy->Writable() ? "" : ", locked");
	return true;
}

void CKernel::SaveFloppy(unsigned nDrive)
{
	FloppyImage *pFloppy = m_pFloppy[nDrive];
	if (!pFloppy->Present() || !pFloppy->Dirty() || !m_bSDMounted)
		return;
	char Path[160];
	StrCopy(Path, kFloppyDir, sizeof Path);
	StrAppend(Path, "/", sizeof Path);
	StrAppend(Path, m_FloppyName[nDrive], sizeof Path);
	pFloppy->PrepareToSave(); // clears Dirty
	if (PiStorage::OverwriteFile(Path, pFloppy->File(), pFloppy->FileSize())) {
		m_FloppyError[nDrive][0] = '\0';
		m_Logger.Write(FromKernel, LogNotice, "Floppy %s saved", m_FloppyName[nDrive]);
	} else {
		StrCopy(m_FloppyError[nDrive], "SAVE FAILED", sizeof m_FloppyError[nDrive]);
		m_Logger.Write(FromKernel, LogError, "Cannot save floppy %s", Path);
	}
}

void CKernel::EjectFloppy(unsigned nDrive)
{
	m_pMac->GetIwm().Remove(nDrive); // finishes any write in progress
	SaveFloppy(nDrive);
	m_pFloppy[nDrive]->Detach();
}

void CKernel::EjectFloppies()
{
	for (unsigned nDrive = 0; nDrive < kFloppyDrives; ++nDrive)
		EjectFloppy(nDrive);
}

void CKernel::CheckFloppies()
{
	if (m_pActive != m_pMac)
		return;
	for (unsigned nDrive = 0; nDrive < kFloppyDrives; ++nDrive) {
		if (!m_pMac->GetIwm().TakeEjected(nDrive))
			continue;
		// The Mac ejected the disk (Put Away, Cmd-E, shutdown...): save it
		// and remember that the drive is empty.
		SaveFloppy(nDrive);
		m_Logger.Write(FromKernel, LogNotice, "%s %s ejected", kFloppyDriveName[nDrive],
			       m_FloppyName[nDrive]);
		m_pFloppy[nDrive]->Detach();
		m_Settings.floppy[nDrive][0] = '\0';
		if (!SaveSettings())
			m_Logger.Write(FromKernel, LogWarning, "Could not save settings");
	}
}

bool CKernel::InsertCdrom()
{
	m_CdromError[0] = '\0';
	if (!m_Settings.cdrom[0] || m_pActive != m_pMac)
		return true;
	char Path[160];
	StrCopy(Path, kCdromDir, sizeof Path);
	StrAppend(Path, "/", sizeof Path);
	StrAppend(Path, m_Settings.cdrom, sizeof Path);
	if (!m_pCdImage->Open(Path, true)) {
		StrCopy(m_CdromError, "cannot open the file", sizeof m_CdromError);
	} else if (m_pCdImage->BlockCount() < 4 * 16 + 4) { // not even an ISO 9660 header
		m_pCdImage->Close();
		StrCopy(m_CdromError, "too small for a CD image", sizeof m_CdromError);
	}
	if (m_CdromError[0]) {
		m_Logger.Write(FromKernel, LogWarning, "CD-ROM %s: %s", m_Settings.cdrom, m_CdromError);
		m_Settings.cdrom[0] = '\0';
		return false;
	}
	m_pMac->GetScsi().InsertCdrom(m_pCdImage);
	m_Logger.Write(FromKernel, LogNotice, "CD-ROM %s inserted (%u MB)", m_Settings.cdrom,
		       m_pCdImage->BlockCount() / 2048);
	return true;
}

void CKernel::EjectCdrom()
{
	if (m_pMac)
		m_pMac->GetScsi().EjectCdrom();
	m_pCdImage->Close();
}

void CKernel::CheckCdrom()
{
	if (m_pActive != m_pMac || !m_pMac->GetScsi().TakeCdromEjected())
		return;
	// The Mac ejected the disc (Put Away, Cmd-E, shutdown...): the drive is
	// empty until a disc is chosen again in the menu.
	m_Logger.Write(FromKernel, LogNotice, "CD-ROM %s ejected", m_Settings.cdrom);
	m_pCdImage->Close();
	m_Settings.cdrom[0] = '\0';
	if (!SaveSettings())
		m_Logger.Write(FromKernel, LogWarning, "Could not save settings");
}
