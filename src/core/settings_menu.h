// The hidden F12 settings overlay. Pure logic + rendering; the kernel feeds it
// HID key presses and decides what to do with the result.
#pragma once

#include "core/rom.h"
#include "core/settings.h"
#include "core/video.h"

namespace pimac {

// ChangeFloppy: put Edited().floppy[ChangedDrive()] in that drive now (or eject
// if it is empty), without restarting the Mac. ChangeCdrom: the same for
// Edited().cdrom and the CD-ROM drive.
// ResetPramAndRestart: as ApplyAndRestart, but the Mac starts with blank
// (default) PRAM, keeping only the clock (asked for twice: Enter confirms).
enum class MenuResult { None, Close, ApplyAndRestart, ResetPramAndRestart, ChangeFloppy, ChangeCdrom,
			ShowDiagnostics, RebootPi };

class SettingsMenu {
public:
	SettingsMenu();

	// romInfo (optional) identifies each file in roms, in the same order:
	// Machine offers the models PiMac runs (one per ROM, plus Test mode),
	// and Memory that model's RAM sizes.
	void Open(const Settings &current, const FileList *roms, const RomInfo *romInfo,
		  const FileList *disks, const FileList *floppies, const FileList *cdroms = 0);
	MenuResult HandleKey(u8 hidUsage); // key presses only
	void Render(Surface &s) const;

	// After ChangeFloppy: what is in the drive now (the menu stays usable).
	void SetFloppyInDrive(unsigned drive, const char *name)
	{
		StrCopy(m_FloppyInDrive[drive], name, sizeof m_FloppyInDrive[drive]);
		StrCopy(m_Edit.floppy[drive], name, sizeof m_Edit.floppy[drive]);
	}
	unsigned ChangedDrive() const { return m_ChangedDrive; }
	// After ChangeCdrom (or when the Mac ejects the disc): what is in the drive.
	void SetCdromInDrive(const char *name)
	{
		StrCopy(m_CdromInDrive, name, sizeof m_CdromInDrive);
		StrCopy(m_Edit.cdrom, name, sizeof m_Edit.cdrom);
	}
	// A one-off message shown in place of the key help until the next key.
	void SetMessage(const char *text) { StrCopy(m_Message, text, sizeof m_Message); }

	const Settings &Edited() const { return m_Edit; }
	unsigned Selected() const { return m_Sel; }

	enum Item {
		ItemMachine, // a ROM in /roms, by the model it runs, or Test mode
		ItemRam,
		ItemCpuSpeed,
		ItemDisk,
		ItemFloppy,
		ItemFloppyExternal,
		ItemCdrom,
		ItemAltKey,
		ItemSound,
		ItemModem,
		ItemPrinter,
		ItemApply,
		ItemResetPram,
		ItemDiagnostics,
		ItemReboot,
		ItemReturn,
		ItemCount
	};

private:
	void Change(int dir);
	void ValueText(unsigned item, char *out, unsigned cap) const;
	static bool IsChoice(unsigned item) { return item < ItemApply; }
	int SelectedRom() const; // index in m_Roms, -1 if none
	bool Offered(unsigned rom) const; // Machine lists this ROM
	void ChangeMachine(int dir);
	RomFamily Family() const; // of the selected ROM
	bool HasCpuSpeed() const; // the selected model's speed can be raised
	static int FloppyDrive(unsigned item); // -1 if not a floppy item
	void ChangeFloppy(unsigned drive, int dir);

	Settings m_Edit;
	const FileList *m_Roms;
	const RomInfo *m_RomInfo;
	const FileList *m_Disks;
	const FileList *m_Floppies;
	const FileList *m_Cdroms;
	char m_CdromInDrive[64];
	char m_FloppyInDrive[kFloppyDrives][64]; // what each drive holds
	unsigned m_ChangedDrive;
	char m_Message[80];
	bool m_ConfirmReset; // Enter was pressed once on Reset PRAM
	unsigned m_Sel;
};

} // namespace pimac
