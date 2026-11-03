// Host-side unit tests for src/core. Build and run with `make test`.
#include <stdio.h>
#include <stdlib.h>

#include "core/input.h"
#include "core/keymap.h"
#include "core/rom.h"
#include "core/settings.h"
#include "core/settings_menu.h"
#include "core/test_machine.h"
#include "core/text_page.h"
#include "core/vhd.h"
#include "core/video.h"

using namespace pimac;

static int g_Failures = 0;
static int g_Checks = 0;

#define CHECK(cond)                                                              \
	do {                                                                     \
		++g_Checks;                                                      \
		if (!(cond)) {                                                   \
			++g_Failures;                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
		}                                                                \
	} while (0)

static void TestKeymap()
{
	CHECK(MacKeyFromHID(0x04, true) == 0x00);  // A
	CHECK(MacKeyFromHID(0x1D, true) == 0x06);  // Z
	CHECK(MacKeyFromHID(0x27, true) == 0x1D);  // 0
	CHECK(MacKeyFromHID(0x28, true) == 0x24);  // Return
	CHECK(MacKeyFromHID(0x2A, true) == 0x33);  // Backspace -> Delete
	CHECK(MacKeyFromHID(0x52, true) == 0x7E);  // Up
	CHECK(MacKeyFromHID(hid::F12, true) == kMacKeyNone); // reserved for menu
	CHECK(MacKeyFromHID(hid::LeftAlt, true) == 0x37);   // Alt -> Command
	CHECK(MacKeyFromHID(hid::LeftGUI, true) == 0x3A);   // Win -> Option
	CHECK(MacKeyFromHID(hid::LeftAlt, false) == 0x3A);
	CHECK(MacKeyFromHID(hid::LeftGUI, false) == 0x37);
	CHECK(MacKeyFromHID(hid::RightShift, true) == 0x38);

	// Every letter maps to a distinct key code.
	bool seen[128] = {};
	for (u8 u = 0x04; u <= 0x1D; ++u) {
		u8 m = MacKeyFromHID(u, true);
		CHECK(m < 128 && !seen[m]);
		if (m < 128)
			seen[m] = true;
	}
}

static void TestKeyboardTracker()
{
	InputQueue q;
	KeyboardTracker t;
	InputEvent e;

	const u8 a[6] = {0x04, 0, 0, 0, 0, 0};
	const u8 ab[6] = {0x04, 0x05, 0, 0, 0, 0};
	const u8 b[6] = {0x05, 0, 0, 0, 0, 0};
	const u8 none[6] = {0, 0, 0, 0, 0, 0};
	const u8 rollover[6] = {0x01, 0x01, 0x01, 0x01, 0x01, 0x01};

	t.Update(0x02, a, q); // Left Shift + A
	CHECK(q.Pop(e) && e.type == EvKeyDown && e.code == hid::LeftShift);
	CHECK(q.Pop(e) && e.type == EvKeyDown && e.code == 0x04);
	CHECK(!q.Pop(e));

	t.Update(0x02, ab, q);
	CHECK(q.Pop(e) && e.type == EvKeyDown && e.code == 0x05);
	CHECK(!q.Pop(e));

	t.Update(0x02, rollover, q); // ignored
	CHECK(!q.Pop(e));

	t.Update(0x00, b, q);
	CHECK(q.Pop(e) && e.type == EvKeyUp && e.code == hid::LeftShift);
	CHECK(q.Pop(e) && e.type == EvKeyUp && e.code == 0x04);
	CHECK(!q.Pop(e));

	t.ReleaseAll(q);
	CHECK(q.Pop(e) && e.type == EvKeyUp && e.code == 0x05);
	CHECK(!q.Pop(e));

	t.Update(0, none, q);
	CHECK(!q.Pop(e));

	// Queue overflow drops rather than corrupts.
	for (unsigned i = 0; i < InputQueue::kSize + 10; ++i)
		PushMouse(q, 1, 1, -1);
	unsigned n = 0;
	while (q.Pop(e)) {
		CHECK(e.type == EvMouse && e.dx == 1 && e.dy == -1 && e.buttons == 1);
		++n;
	}
	CHECK(n == InputQueue::kSize - 1);
}

static void TestSettings()
{
	Settings s;
	SettingsDefaults(s);
	CHECK(StrEqual(s.machine, "classic"));
	CHECK(s.ramKB == 4096);
	CHECK(s.altIsCommand);

	const char *text = "# comment\r\n  machine = se \r\nram_kb=2560\nrom=Mac Classic.rom\n"
			   "disk=System 7.vhd\nalt_is_command=0\nbogus=1\nram_kb=3000\nmachine=amiga\n";
	SettingsParse(s, text, (unsigned)strlen(text));
	CHECK(StrEqual(s.machine, "se"));
	CHECK(s.ramKB == 2560);
	CHECK(StrEqual(s.rom, "Mac Classic.rom"));
	CHECK(StrEqual(s.disk, "System 7.vhd"));
	CHECK(!s.altIsCommand);

	char buf[512];
	unsigned n = SettingsSerialize(s, buf, sizeof buf);
	CHECK(n == strlen(buf));
	Settings r;
	SettingsDefaults(r);
	SettingsParse(r, buf, n);
	CHECK(StrEqual(r.machine, s.machine) && r.ramKB == s.ramKB && StrEqual(r.rom, s.rom) &&
	      StrEqual(r.disk, s.disk) && r.altIsCommand == s.altIsCommand);

	FileList l;
	FileListClear(l);
	FileListAdd(l, "zeta.vhd");
	FileListAdd(l, "Alpha.vhd");
	FileListAdd(l, "beta.vhd");
	CHECK(l.count == 3 && StrEqual(l.names[0], "Alpha.vhd") && StrEqual(l.names[2], "zeta.vhd"));
	CHECK(FileListFind(l, "beta.vhd") == 1 && FileListFind(l, "nope") == -1);
}

static void TestMenu()
{
	Settings s;
	SettingsDefaults(s);
	FileList roms, disks;
	FileListClear(roms);
	FileListClear(disks);
	FileListAdd(roms, "Classic.rom");
	FileListAdd(roms, "Plus.rom");
	FileListAdd(disks, "System7.vhd");

	SettingsMenu m;
	m.Open(s, &roms, &disks);
	CHECK(m.HandleKey(hid::Right) == MenuResult::None); // machine -> next
	CHECK(StrEqual(m.Edited().machine, "plus"));
	CHECK(m.HandleKey(hid::Left) == MenuResult::None);
	CHECK(StrEqual(m.Edited().machine, "classic"));

	m.HandleKey(hid::Down); // RAM
	m.HandleKey(hid::Right);
	CHECK(m.Edited().ramKB == 1024); // wraps from 4096

	m.HandleKey(hid::Down); // ROM
	m.HandleKey(hid::Right);
	CHECK(StrEqual(m.Edited().rom, "Classic.rom"));
	m.HandleKey(hid::Right);
	CHECK(StrEqual(m.Edited().rom, "Plus.rom"));

	m.HandleKey(hid::Down); // disk
	m.HandleKey(hid::Right);
	CHECK(StrEqual(m.Edited().disk, "System7.vhd"));
	m.HandleKey(hid::Right);
	CHECK(m.Edited().disk[0] == '\0');

	m.HandleKey(hid::Down); // alt key
	m.HandleKey(hid::Enter);
	CHECK(!m.Edited().altIsCommand);

	m.HandleKey(hid::Down);
	CHECK(m.Selected() == SettingsMenu::ItemApply);
	CHECK(m.HandleKey(hid::Enter) == MenuResult::ApplyAndRestart);
	m.HandleKey(hid::Down);
	CHECK(m.Selected() == SettingsMenu::ItemDiagnostics);
	CHECK(m.HandleKey(hid::Enter) == MenuResult::ShowDiagnostics);
	m.HandleKey(hid::Down);
	CHECK(m.HandleKey(hid::Enter) == MenuResult::RebootPi);
	m.HandleKey(hid::Down);
	CHECK(m.HandleKey(hid::Enter) == MenuResult::Close);
	m.HandleKey(hid::Down); // wraps to top
	CHECK(m.Selected() == SettingsMenu::ItemMachine);
	CHECK(m.HandleKey(hid::Escape) == MenuResult::Close);
	CHECK(m.HandleKey(hid::F12) == MenuResult::Close);

	// The original settings object is untouched until applied.
	CHECK(s.altIsCommand && s.ramKB == 4096);
}

static void TestRomAndVhd()
{
	const u8 classic[4] = {0xA4, 0x9F, 0x99, 0x14};
	RomInfo r = IdentifyRom(classic, 4);
	CHECK(r.known && r.checksum == 0xA49F9914 && StrEqual(r.model, "Macintosh Classic"));
	const u8 junk[4] = {1, 2, 3, 4};
	CHECK(!IdentifyRom(junk, 4).known);
	CHECK(!IdentifyRom(classic, 2).known);

	u8 f[kVhdFooterSize] = {};
	memcpy(f, "conectix", 8);
	const u64 size = 40ull * 1024 * 1024;
	for (int i = 0; i < 8; ++i)
		f[48 + i] = (u8)(size >> (56 - 8 * i));
	f[63] = VhdFixed;
	u32 sum = 0;
	for (unsigned i = 0; i < kVhdFooterSize; ++i)
		sum += f[i];
	u32 ck = ~sum;
	f[64] = (u8)(ck >> 24); f[65] = (u8)(ck >> 16); f[66] = (u8)(ck >> 8); f[67] = (u8)ck;

	VhdInfo v = ParseVhdFooter(f);
	CHECK(v.valid && v.checksumOk && v.diskType == VhdFixed && v.currentSize == size);
	f[100] ^= 1;
	CHECK(!ParseVhdFooter(f).checksumOk);
	f[0] = 'X';
	CHECK(!ParseVhdFooter(f).valid);
}

static void TestScaler()
{
	static u8 fb[1024 * 768];
	memset(fb, 0xEE, sizeof fb);
	Surface s = {fb, 1024, 768, 1024};

	static MonoScaler sc; // large; keep off the stack
	sc.Configure(s, 512, 342);
	CHECK(sc.Scale() == 2 && sc.OffsetX() == 0 && sc.OffsetY() == 42);

	static u8 bits[64 * 342] = {};
	bits[0] = 0x80;          // top-left pixel black
	bits[64 * 341 + 63] = 1; // bottom-right pixel black
	MonoFrame f = {bits, 512, 342, 64};

	CHECK(sc.Present(f) == 342);
	CHECK(fb[0] == PalBorder && fb[41 * 1024 + 500] == PalBorder);   // top bar
	CHECK(fb[726 * 1024 + 10] == PalBorder);                          // bottom bar
	CHECK(fb[42 * 1024 + 0] == PalBlack && fb[43 * 1024 + 1] == PalBlack);
	CHECK(fb[42 * 1024 + 2] == PalWhite);
	CHECK(fb[(42 + 683) * 1024 + 1023] == PalBlack);
	CHECK(fb[(42 + 683) * 1024 + 1021] == PalWhite);

	CHECK(sc.Present(f) == 0); // nothing changed
	bits[64 * 100] = 0xFF;
	CHECK(sc.Present(f) == 1);
	CHECK(fb[(42 + 200) * 1024 + 15] == PalBlack && fb[(42 + 201) * 1024 + 16] == PalWhite);
	sc.Invalidate();
	CHECK(sc.Present(f) == 342);
}

static void TestTestMachine()
{
	static TestMachine m;
	MonoFrame f = m.Screen();
	CHECK(f.width == 512 && f.height == 342 && f.rowBytes == 64);
	m.MouseMotion(-10000, -10000);
	m.MouseMotion(5, 5);
	m.RunFrame();
	// Arrow tip at (4,4) after clamp to 0 then +5 (hotspot offset 1).
	CHECK((f.bits[5 * 64] & (0x80 >> 5)) != 0);
}

static void TestTextPage()
{
	static TextPage p;
	const char log[] = "\x1b[97m00:00:01.20 usbdev0-1: Device ven93a-2510 found\x1b[0m\r\n"
			   "two\tparts\nthree";
	p.AddText(log, sizeof log - 1);
	CHECK(p.LineCount() == 3);
	CHECK(StrEqual(p.Line(0), "00:00:01.20 usbdev0-1: Device ven93a-2510 found"));
	CHECK(StrEqual(p.Line(1), "two parts"));
	CHECK(StrEqual(p.Line(2), "three"));

	// Overflow keeps the newest lines.
	p.Clear();
	for (unsigned i = 0; i < TextPage::kMaxLines + 5; ++i) {
		char line[16] = "";
		StrAppendUInt(line, i, sizeof line);
		p.AddLine(line);
	}
	CHECK(p.LineCount() == TextPage::kMaxLines);
	CHECK(StrEqual(p.Line(0), "5"));

	// Scrolling clamps to the content.
	static u8 fb[1024 * 768];
	Surface s = {fb, 1024, 768, 1024};
	p.Render(s, "Test");
	unsigned visible = TextPage::VisibleRows(s);
	CHECK(visible > 30);
	CHECK(!p.HandleKey(0x4D)); // End
	CHECK(p.Top() == TextPage::kMaxLines - visible);
	CHECK(!p.HandleKey(hid::Down));
	CHECK(p.Top() == TextPage::kMaxLines - visible);
	CHECK(!p.HandleKey(0x4A)); // Home
	CHECK(p.Top() == 0);
	CHECK(!p.HandleKey(hid::Up));
	CHECK(p.Top() == 0);
	CHECK(p.HandleKey(hid::Escape));
}

int main()
{
	TestKeymap();
	TestKeyboardTracker();
	TestSettings();
	TestMenu();
	TestRomAndVhd();
	TestScaler();
	TestTestMachine();
	TestTextPage();
	printf("%d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
