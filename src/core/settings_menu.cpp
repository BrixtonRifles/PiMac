#include "core/settings_menu.h"
#include "core/draw.h"
#include "core/keymap.h"

namespace pimac {

namespace {

const char *const kLabels[SettingsMenu::ItemCount] = {
	"Machine", "Memory", "ROM", "Hard disk", "Alt key",
	"Save & restart Mac", "USB diagnostics", "Restart Raspberry Pi", "Return to Mac",
};

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

SettingsMenu::SettingsMenu() : m_Roms(0), m_Disks(0), m_Sel(0)
{
	SettingsDefaults(m_Edit);
}

void SettingsMenu::Open(const Settings &current, const FileList *roms, const FileList *disks)
{
	m_Edit = current;
	m_Roms = roms;
	m_Disks = disks;
	m_Sel = 0;
}

void SettingsMenu::Change(int dir)
{
	switch (m_Sel) {
	case ItemMachine: {
		int cur = 0;
		for (unsigned i = 0; i < kMachineModelCount; ++i)
			if (StrEqual(kMachineModels[i].id, m_Edit.machine))
				cur = (int)i;
		cur = Wrap(cur + dir, (int)kMachineModelCount);
		StrCopy(m_Edit.machine, kMachineModels[cur].id, sizeof m_Edit.machine);
		break;
	}
	case ItemRam: {
		int cur = 0;
		for (unsigned i = 0; i < kRamOptionCount; ++i)
			if (kRamOptionsKB[i] == m_Edit.ramKB)
				cur = (int)i;
		m_Edit.ramKB = kRamOptionsKB[Wrap(cur + dir, (int)kRamOptionCount)];
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
	case ItemAltKey:
		m_Edit.altIsCommand = !m_Edit.altIsCommand;
		break;
	default:
		break;
	}
}

MenuResult SettingsMenu::HandleKey(u8 usage)
{
	switch (usage) {
	case hid::Up:
		m_Sel = (unsigned)Wrap((int)m_Sel - 1, ItemCount);
		break;
	case hid::Down:
		m_Sel = (unsigned)Wrap((int)m_Sel + 1, ItemCount);
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
		if (IsChoice(m_Sel)) {
			Change(+1);
			break;
		}
		if (m_Sel == ItemApply)
			return MenuResult::ApplyAndRestart;
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
	case ItemMachine:
		StrCopy(out, MachineName(m_Edit.machine), cap);
		break;
	case ItemRam:
		StrAppendUInt(out, m_Edit.ramKB / 1024, cap);
		if (m_Edit.ramKB % 1024)
			StrAppend(out, ".5", cap);
		StrAppend(out, " MB", cap);
		break;
	case ItemRom:
		StrCopy(out, m_Edit.rom[0] ? m_Edit.rom
			     : (m_Roms && m_Roms->count ? m_Roms->names[0] : "(none found)"), cap);
		break;
	case ItemDisk:
		StrCopy(out, m_Edit.disk[0] ? m_Edit.disk : "(none)", cap);
		break;
	case ItemAltKey:
		StrCopy(out, m_Edit.altIsCommand ? "Command" : "Option", cap);
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
	DrawText(s, x0 + kPanelW - kPad - 8 * 4, y0 + 22, "v0.1", PalPanelDim, -1, 1);
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

	DrawText(s, x0 + kPad, y0 + panelH - 28,
		 "Up/Down select   Left/Right change   Enter choose   F12/Esc back",
		 PalPanelDim, -1, 1);
}

} // namespace pimac
