// The hidden F12 settings overlay. Pure logic + rendering; the kernel feeds it
// HID key presses and decides what to do with the result.
#pragma once

#include "core/settings.h"
#include "core/video.h"

namespace pimac {

// ChangeFloppy: put Edited().floppy[ChangedDrive()] in that drive now (or eject
// if it is empty), without restarting the Mac.
enum class MenuResult { None, Close, ApplyAndRestart, ChangeFloppy, ShowDiagnostics, RebootPi };

class SettingsMenu {
public:
	SettingsMenu();

	void Open(const Settings &current, const FileList *roms, const FileList *disks,
		  const FileList *floppies);
	MenuResult HandleKey(u8 hidUsage); // key presses only
	void Render(Surface &s) const;

	// After ChangeFloppy: what is in the drive now (the menu stays usable).
	void SetFloppyInDrive(unsigned drive, const char *name)
	{
		StrCopy(m_FloppyInDrive[drive], name, sizeof m_FloppyInDrive[drive]);
		StrCopy(m_Edit.floppy[drive], name, sizeof m_Edit.floppy[drive]);
	}
	unsigned ChangedDrive() const { return m_ChangedDrive; }
	// A one-off message shown in place of the key help until the next key.
	void SetMessage(const char *text) { StrCopy(m_Message, text, sizeof m_Message); }

	const Settings &Edited() const { return m_Edit; }
	unsigned Selected() const { return m_Sel; }

	enum Item {
		ItemMachine,
		ItemRam,
		ItemRom,
		ItemDisk,
		ItemFloppy,
		ItemFloppyExternal,
		ItemAltKey,
		ItemSound,
		ItemApply,
		ItemDiagnostics,
		ItemReboot,
		ItemReturn,
		ItemCount
	};

private:
	void Change(int dir);
	void ValueText(unsigned item, char *out, unsigned cap) const;
	static bool IsChoice(unsigned item) { return item < ItemApply; }
	static int FloppyDrive(unsigned item); // -1 if not a floppy item
	void ChangeFloppy(unsigned drive, int dir);

	Settings m_Edit;
	const FileList *m_Roms;
	const FileList *m_Disks;
	const FileList *m_Floppies;
	char m_FloppyInDrive[kFloppyDrives][64]; // what each drive holds
	unsigned m_ChangedDrive;
	char m_Message[80];
	unsigned m_Sel;
};

} // namespace pimac
