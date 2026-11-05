#include "core/settings_menu.h"
#include "core/draw.h"
#include "core/keymap.h"

namespace pimac {

namespace {

const char *const kLabels[] = {
	"ROM", "Machine", "Memory", "CPU speed", "Hard disk", "Floppy", "Ext. floppy", "CD-ROM", "Alt key", "Sound",
	"Modem port", "Printer", // fit before the value column
	"Save & restart Mac", "Reset PRAM & restart", "Diagnostics", "Restart Raspberry Pi", "Return to Mac",
};
static_assert(sizeof kLabels / sizeof kLabels[0] == SettingsMenu::ItemCount,
	      "one label per menu item");

const unsigned kScale = 2;               // 16px characters
const int kCharW = 8 * kScale;
const int kPanelW = 42 * kCharW;         // 672
const int kRowH = 28;
const int kPad = 24;
const int kValueCol = kPad + 12 * kCharW; // x offset of the value column

int Wrap(int v, int n)
{
	return n <= 0 ? 0 : ((v % n) + n) % n;
}

} // namespace

SettingsMenu::SettingsMenu()
	: m_Roms(0), m_RomInfo(0), m_Disks(0), m_Floppies(0), m_Cdroms(0), m_ChangedDrive(0), m_Sel(0)
{
	m_CdromInDrive[0] = '\0';
	for (unsigned d = 0; d < kFloppyDrives; ++d)
		m_FloppyInDrive[d][0] = '\0';
	m_Message[0] = '\0';
	m_ConfirmReset = false;
	SettingsDefaults(m_Edit);
}

void SettingsMenu::Open(const Settings &current, const FileList *roms, const RomInfo *romInfo,
			const FileList *disks, const FileList *floppies, const FileList *cdroms)
{
	m_Edit = current;
	m_Roms = roms;
	m_RomInfo = romInfo;
	m_Disks = disks;
	m_Floppies = floppies;
	m_Cdroms = cdroms;
	StrCopy(m_CdromInDrive, current.cdrom, sizeof m_CdromInDrive);
	for (unsigned d = 0; d < kFloppyDrives; ++d)
		StrCopy(m_FloppyInDrive[d], current.floppy[d], sizeof m_FloppyInDrive[d]);
	m_Message[0] = '\0';
	m_ConfirmReset = false;
	m_Sel = 0;
}

int SettingsMenu::SelectedRom() const
{
	if (!m_Roms || !m_Roms->count)
		return -1;
	if (!m_Edit.rom[0])
		return DefaultRom(m_RomInfo, m_Roms->count); // as the kernel
	return FileListFind(*m_Roms, m_Edit.rom);
}

RomFamily SettingsMenu::Family() const
{
	const int rom = SelectedRom();
	return rom >= 0 && m_RomInfo ? m_RomInfo[rom].family : RomNotEmulated;
}

bool SettingsMenu::HasCpuSpeed() const
{
	unsigned count = 0;
	CpuSpeedOptions(Family(), count);
	return count > 1;
}

int SettingsMenu::FloppyDrive(unsigned item)
{
	return item == ItemFloppy ? 0 : item == ItemFloppyExternal ? 1 : -1;
}

void SettingsMenu::ChangeFloppy(unsigned drive, int dir)
{
	if (!m_Floppies)
		return;
	// Position 0 is "(empty)", 1..count are files. A disk can be in only one
	// drive, so skip the one in the other drive.
	const char *other = m_FloppyInDrive[1 - drive];
	char *edit = m_Edit.floppy[drive];
	const int n = (int)m_Floppies->count + 1;
	int cur = edit[0] ? FileListFind(*m_Floppies, edit) + 1 : 0;
	for (int tries = 0; tries < n; ++tries) {
		cur = Wrap(cur + dir, n);
		if (!cur || !StrEqual(m_Floppies->names[cur - 1], other))
			break;
	}
	StrCopy(edit, cur ? m_Floppies->names[cur - 1] : "", sizeof m_Edit.floppy[drive]);
}

void SettingsMenu::Change(int dir)
{
	switch (m_Sel) {
	case ItemRam: {
		// Only the sizes this ROM's model supports, starting from the size
		// it would actually get.
		unsigned count = 0;
		const unsigned *sizes = RamOptionsKB(Family(), count);
		const unsigned fitted = FitRamKB(Family(), m_Edit.ramKB);
		int cur = 0;
		for (unsigned i = 0; i < count; ++i)
			if (sizes[i] == fitted)
				cur = (int)i;
		m_Edit.ramKB = sizes[Wrap(cur + dir, (int)count)];
		break;
	}
	case ItemCpuSpeed: {
		if (!HasCpuSpeed())
			break; // the Classic runs at its own speed only
		unsigned count = 0;
		const unsigned *speeds = CpuSpeedOptions(Family(), count);
		const unsigned fitted = FitCpuPercent(Family(), m_Edit.cpuPercent);
		int cur = 0;
		for (unsigned i = 0; i < count; ++i)
			if (speeds[i] == fitted)
				cur = (int)i;
		m_Edit.cpuPercent = speeds[Wrap(cur + dir, (int)count)];
		break;
	}
	case ItemRom:
		if (m_Roms && m_Roms->count) {
			int cur = FileListFind(*m_Roms, m_Edit.rom);
			if (cur < 0)
				cur = dir > 0 ? -1 : 0;
			StrCopy(m_Edit.rom, m_Roms->names[Wrap(cur + dir, (int)m_Roms->count)],
				sizeof m_Edit.rom);
		}
		break;
	case ItemDisk:
		if (m_Disks) {
			// Position 0 is "(none)", 1..count are files.
			int cur = m_Edit.disk[0] ? FileListFind(*m_Disks, m_Edit.disk) + 1 : 0;
			cur = Wrap(cur + dir, (int)m_Disks->count + 1);
			StrCopy(m_Edit.disk, cur ? m_Disks->names[cur - 1] : "", sizeof m_Edit.disk);
		}
		break;
	case ItemFloppy:
	case ItemFloppyExternal:
		ChangeFloppy((unsigned)FloppyDrive(m_Sel), dir);
		break;
	case ItemCdrom:
		if (m_Cdroms) {
			// Position 0 is "(empty)", 1..count are files.
			int cur = m_Edit.cdrom[0] ? FileListFind(*m_Cdroms, m_Edit.cdrom) + 1 : 0;
			cur = Wrap(cur + dir, (int)m_Cdroms->count + 1);
			StrCopy(m_Edit.cdrom, cur ? m_Cdroms->names[cur - 1] : "", sizeof m_Edit.cdrom);
		}
		break;
	case ItemAltKey:
		m_Edit.altIsCommand = !m_Edit.altIsCommand;
		break;
	case ItemSound:
		m_Edit.sound = (unsigned)Wrap((int)m_Edit.sound + dir, SoundOutputCount);
		break;
	case ItemModem:
		m_Edit.modem = !m_Edit.modem;
		break;
	case ItemPrinter:
		m_Edit.localTalk = !m_Edit.localTalk;
		break;
	default:
		break;
	}
}

MenuResult SettingsMenu::HandleKey(u8 usage)
{
	m_Message[0] = '\0';
	const bool confirming = m_ConfirmReset;
	m_ConfirmReset = false; // any other key cancels
	switch (usage) {
	case hid::Up:
		do
			m_Sel = (unsigned)Wrap((int)m_Sel - 1, ItemCount);
		while (!IsSelectable(m_Sel));
		break;
	case hid::Down:
		do
			m_Sel = (unsigned)Wrap((int)m_Sel + 1, ItemCount);
		while (!IsSelectable(m_Sel));
		break;
	case hid::Left:
		if (IsChoice(m_Sel))
			Change(-1);
		break;
	case hid::Right:
		if (IsChoice(m_Sel))
			Change(+1);
		break;
	case hid::Enter:
	case hid::KeypadEnter:
	case hid::Space:
		if (FloppyDrive(m_Sel) >= 0) {
			// Enter acts at once: insert the chosen floppy, or eject the
			// one in the drive if the choice is unchanged.
			const unsigned d = (unsigned)FloppyDrive(m_Sel);
			char *edit = m_Edit.floppy[d];
			if (StrEqual(edit, m_FloppyInDrive[d]) && m_FloppyInDrive[d][0])
				edit[0] = '\0';
			if (!StrEqual(edit, m_FloppyInDrive[d])) {
				m_ChangedDrive = d;
				return MenuResult::ChangeFloppy;
			}
			Change(+1);
			break;
		}
		if (m_Sel == ItemCdrom) {
			// Like a floppy: insert the chosen disc, or eject the one in.
			if (StrEqual(m_Edit.cdrom, m_CdromInDrive) && m_CdromInDrive[0])
				m_Edit.cdrom[0] = '\0';
			if (!StrEqual(m_Edit.cdrom, m_CdromInDrive))
				return MenuResult::ChangeCdrom;
			Change(+1);
			break;
		}
		if (IsChoice(m_Sel)) {
			Change(+1);
			break;
		}
		if (m_Sel == ItemApply)
			return MenuResult::ApplyAndRestart;
		if (m_Sel == ItemResetPram) {
			if (confirming)
				return MenuResult::ResetPramAndRestart;
			m_ConfirmReset = true;
			SetMessage("Press Enter again to reset PRAM and restart (any other key cancels)");
			break;
		}
		if (m_Sel == ItemDiagnostics)
			return MenuResult::ShowDiagnostics;
		if (m_Sel == ItemReboot)
			return MenuResult::RebootPi;
		return MenuResult::Close;
	case hid::Escape:
	case hid::F12:
		return MenuResult::Close;
	default:
		break;
	}
	return MenuResult::None;
}

void SettingsMenu::ValueText(unsigned item, char *out, unsigned cap) const
{
	out[0] = '\0';
	switch (item) {
	case ItemMachine: {
		const int rom = SelectedRom();
		if (rom < 0) {
			StrCopy(out, "(no ROM)", cap);
		} else if (!m_RomInfo) {
			StrCopy(out, "(set by the ROM)", cap);
		} else {
			StrCopy(out, m_RomInfo[rom].model, cap);
			if (m_RomInfo[rom].family == RomNotEmulated)
				StrAppend(out, " - not emulated", cap);
		}
		break;
	}
	case ItemRam: {
		const unsigned kb = FitRamKB(Family(), m_Edit.ramKB);
		StrAppendUInt(out, kb / 1024, cap);
		if (kb % 1024)
			StrAppend(out, ".5", cap);
		StrAppend(out, " MB", cap);
		break;
	}
	case ItemCpuSpeed:
		StrAppendUInt(out, FitCpuPercent(Family(), m_Edit.cpuPercent), cap);
		StrAppend(out, HasCpuSpeed() ? "%" : "% (fixed)", cap);
		break;
	case ItemRom:
		StrCopy(out, m_Edit.rom[0] ? m_Edit.rom
			     : (m_Roms && m_Roms->count ? m_Roms->names[0] : "(none found)"), cap);
		break;
	case ItemDisk:
		StrCopy(out, m_Edit.disk[0] ? m_Edit.disk : "(none)", cap);
		break;
	case ItemFloppy:
	case ItemFloppyExternal: {
		const char *name = m_Edit.floppy[FloppyDrive(item)];
		StrCopy(out, name[0] ? name : "(empty)", cap);
		break;
	}
	case ItemCdrom:
		StrCopy(out, m_Edit.cdrom[0] ? m_Edit.cdrom : "(empty)", cap);
		break;
	case ItemAltKey:
		StrCopy(out, m_Edit.altIsCommand ? "Command" : "Option", cap);
		break;
	case ItemSound:
		StrCopy(out, SoundOutputName(m_Edit.sound), cap);
		break;
	case ItemModem:
		StrCopy(out, m_Edit.modem ? "Network modem" : "Off", cap);
		break;
	case ItemPrinter:
		StrCopy(out, m_Edit.localTalk ? "LocalTalk network" : "Off", cap);
		break;
	default:
		break;
	}
}

void SettingsMenu::Render(Surface &s) const
{
	const int titleH = 56;
	const int panelH = titleH + ItemCount * kRowH + 12 + 40;
	const int x0 = ((int)s.width - kPanelW) / 2;
	const int y0 = ((int)s.height - panelH) / 2;

	FillRect(s, x0, y0, kPanelW, panelH, PalPanel);
	DrawFrame(s, x0, y0, kPanelW, panelH, PalPanelText, 2);
	DrawText(s, x0 + kPad, y0 + 18, "PiMac Settings", PalPanelText, -1, kScale);
	DrawText(s, x0 + kPanelW - kPad - 8 * 4, y0 + 22, "v0.3", PalPanelDim, -1, 1);
	FillRect(s, x0 + kPad, y0 + titleH - 10, kPanelW - 2 * kPad, 2, PalPanelDim);

	// Value column fits this many characters before truncation.
	const unsigned maxChars = (unsigned)((kPanelW - kValueCol - kPad) / kCharW) - 4;

	for (unsigned i = 0; i < ItemCount; ++i) {
		int y = y0 + titleH + (int)i * kRowH + (i >= ItemApply ? 12 : 0);
		bool sel = (i == m_Sel);
		u8 fg = sel ? PalHighlightText : PalPanelText;
		if (sel)
			FillRect(s, x0 + 8, y - 6, kPanelW - 16, kRowH, PalHighlight);

		DrawText(s, x0 + kPad, y, kLabels[i], fg, -1, kScale);
		if (i == ItemMachine) { // read-only, follows the ROM
			char model[FileList::kNameLen + 8];
			ValueText(i, model, sizeof model);
			if (strlen(model) > maxChars + 4)
				model[maxChars + 4] = '\0';
			DrawText(s, x0 + kValueCol + 2 * kCharW, y, model, PalPanelDim, -1, kScale);
			continue;
		}
		if (!IsChoice(i))
			continue;

		char value[FileList::kNameLen + 8];
		ValueText(i, value, sizeof value);
		if (strlen(value) > maxChars) {
			value[maxChars - 2] = '.';
			value[maxChars - 1] = '.';
			value[maxChars] = '\0';
		}
		char shown[FileList::kNameLen + 16];
		StrCopy(shown, "< ", sizeof shown);
		StrAppend(shown, value, sizeof shown);
		StrAppend(shown, " >", sizeof shown);
		DrawText(s, x0 + kValueCol, y, shown, fg, -1, kScale);
	}

	const char *help = "Up/Down select   Left/Right change   Enter choose   F12/Esc back";
	if (FloppyDrive(m_Sel) >= 0) {
		const unsigned d = (unsigned)FloppyDrive(m_Sel);
		if (!StrEqual(m_Edit.floppy[d], m_FloppyInDrive[d]))
			help = m_Edit.floppy[d][0] ? "Enter inserts this floppy now (no restart needed)"
						   : "Enter ejects the floppy now";
		else if (m_FloppyInDrive[d][0])
			help = "Enter ejects this floppy   Left/Right pick another";
		else
			help = "Put .dsk images in /floppies   Left/Right pick one";
	}
	if (m_Sel == ItemCdrom) {
		if (!StrEqual(m_Edit.cdrom, m_CdromInDrive))
			help = m_Edit.cdrom[0] ? "Enter inserts this CD now (no restart needed)"
					       : "Enter ejects the CD now";
		else if (m_CdromInDrive[0])
			help = "Enter ejects this CD   Left/Right pick another";
		else
			help = "Put .toast/.iso images in /cdroms   Left/Right pick one";
	}
	if (m_Sel == ItemResetPram)
		help = "Sets this Mac's PRAM back to defaults (e.g. 24-bit mode); keeps the clock";
	if (m_Sel == ItemCpuSpeed)
		help = HasCpuSpeed() ? (Family() == RomColorClassic2
						? "Pi 3: 75% keeps real time. Above 100% uses more Pi CPU. Needs restart"
						: "Above 100% helps heavy games; uses more Pi CPU. Needs restart")
				     : "Only the 68030 Macs (Classic II, Color Classic I/II) can run faster";
	if (m_Message[0])
		DrawText(s, x0 + kPad, y0 + panelH - 28, m_Message, PalPanelText, -1, 1);
	else
		DrawText(s, x0 + kPad, y0 + panelH - 28, help, PalPanelDim, -1, 1);
}

} // namespace pimac
