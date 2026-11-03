// The hidden F12 settings overlay. Pure logic + rendering; the kernel feeds it
// HID key presses and decides what to do with the result.
#pragma once

#include "core/settings.h"
#include "core/video.h"

namespace pimac {

enum class MenuResult { None, Close, ApplyAndRestart, RebootPi };

class SettingsMenu {
public:
	SettingsMenu();

	void Open(const Settings &current, const FileList *roms, const FileList *disks);
	MenuResult HandleKey(u8 hidUsage); // key presses only
	void Render(Surface &s) const;

	const Settings &Edited() const { return m_Edit; }
	unsigned Selected() const { return m_Sel; }

	enum Item {
		ItemMachine,
		ItemRam,
		ItemRom,
		ItemDisk,
		ItemAltKey,
		ItemApply,
		ItemReboot,
		ItemReturn,
		ItemCount
	};

private:
	void Change(int dir);
	void ValueText(unsigned item, char *out, unsigned cap) const;
	static bool IsChoice(unsigned item) { return item < ItemApply; }

	Settings m_Edit;
	const FileList *m_Roms;
	const FileList *m_Disks;
	unsigned m_Sel;
};

} // namespace pimac
