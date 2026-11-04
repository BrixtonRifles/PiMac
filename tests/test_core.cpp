// Host-side unit tests for src/core. Build and run with `make test`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "core/audio.h"
#include "core/floppy_image.h"
#include "core/mfm.h"
#include "core/gcr.h"
#include "core/input.h"
#include "core/keymap.h"
#include "core/macclock.h"
#include "core/rom.h"
#include "core/settings.h"
#include "core/settings_menu.h"
#include "core/test_machine.h"
#include "core/text_page.h"
#include "core/vhd.h"
#include "core/video.h"
#include "mac/asc.h"
#include "mac/localtalk.h"
#include "mac/scsi.h"
#include "mac/modem.h"
#include "mac/scc.h"

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
	CHECK(s.ramKB == 4096);
	CHECK(s.altIsCommand);
	CHECK(s.floppy[0][0] == '\0' && s.floppy[1][0] == '\0' && s.cdrom[0] == '\0');
	CHECK(s.modem && s.localTalk && s.modemListen == 0);

	const char *text = "# comment\r\n  machine = se \r\nram_kb=2048\nrom=Mac Classic.rom\n"
			   "disk=System 7.vhd\nalt_is_command=0\nbogus=1\nram_kb=3000\nmachine=amiga\n"
			   "sound=jack\nsound=loud\nfloppy=System Tools.dsk\nfloppy_external=Games.img\n"
			   "modem=off\nmodem_listen=6400\nlocaltalk=0\nmodem_listen=70000\ncdrom=Civ II.toast\n";
	SettingsParse(s, text, (unsigned)strlen(text));
	CHECK(s.ramKB == 2048); // "machine" lines (older versions) are ignored
	CHECK(StrEqual(s.rom, "Mac Classic.rom"));
	CHECK(StrEqual(s.disk, "System 7.vhd"));
	CHECK(!s.altIsCommand);
	CHECK(s.sound == SoundJack);
	CHECK(StrEqual(s.floppy[0], "System Tools.dsk") && StrEqual(s.floppy[1], "Games.img"));
	CHECK(!s.modem && !s.localTalk && s.modemListen == 6400);
	CHECK(StrEqual(s.cdrom, "Civ II.toast"));

	char buf[512];
	unsigned n = SettingsSerialize(s, buf, sizeof buf);
	CHECK(n == strlen(buf));
	Settings r;
	SettingsDefaults(r);
	SettingsParse(r, buf, n);
	CHECK(strstr(buf, "machine") == 0);
	CHECK(r.ramKB == s.ramKB && StrEqual(r.rom, s.rom) &&
	      StrEqual(r.disk, s.disk) && r.altIsCommand == s.altIsCommand && r.sound == s.sound &&
	      StrEqual(r.floppy[0], s.floppy[0]) && StrEqual(r.floppy[1], s.floppy[1]) &&
	      StrEqual(r.cdrom, s.cdrom) &&
	      r.modem == s.modem && r.localTalk == s.localTalk && r.modemListen == s.modemListen);

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
	FileListAdd(roms, "Classic II.rom");
	FileListAdd(roms, "Plus.rom");
	static const u8 kClassic[4] = {0xA4, 0x9F, 0x99, 0x14};
	static const u8 kClassic2[4] = {0x31, 0x93, 0x67, 0x0E};
	static const u8 kPlus[4] = {0x4D, 0x1F, 0x81, 0x72};
	CHECK(StrEqual(roms.names[0], "Classic II.rom") && StrEqual(roms.names[1], "Classic.rom"));
	const RomInfo romInfo[3] = {IdentifyRom(kClassic2, 4), IdentifyRom(kClassic, 4),
				    IdentifyRom(kPlus, 4)};
	CHECK(romInfo[1].family == RomCompact && romInfo[0].family == RomClassic2 &&
	      romInfo[2].family == RomNotEmulated);
	CHECK(FitRamKB(RomCompact, 6144) == 4096 && FitRamKB(RomClassic2, 1024) == 2048 &&
	      FitRamKB(RomClassic2, 5000) == 6144 && FitRamKB(RomCompact, 2048) == 2048);
	static const u8 kColorClassic[4] = {0xEC, 0xD9, 0x9D, 0xC0};
	CHECK(IdentifyRom(kColorClassic, 4).family == RomColorClassic);
	CHECK(FitRamKB(RomColorClassic, 2048) == 4096 && FitRamKB(RomColorClassic, 7000) == 8192 &&
	      FitRamKB(RomColorClassic, 10240) == 10240 && FitRamKB(RomClassic2, 8192) == 10240);
	FileListAdd(disks, "System7.vhd");
	FileList floppies;
	FileListClear(floppies);
	FileListAdd(floppies, "Disk A.dsk");
	FileListAdd(floppies, "Disk B.dsk");
	FileList cdroms;
	FileListClear(cdroms);
	FileListAdd(cdroms, "Game.toast");

	SettingsMenu m;
	m.Open(s, &roms, romInfo, &disks, &floppies, &cdroms);
	CHECK(m.Selected() == SettingsMenu::ItemRom);
	m.HandleKey(hid::Right); // ROM: "" (= first) -> the first, named
	CHECK(StrEqual(m.Edited().rom, "Classic II.rom"));

	m.HandleKey(hid::Down); // skips the read-only Machine line
	CHECK(m.Selected() == SettingsMenu::ItemRam);
	m.HandleKey(hid::Right);
	CHECK(m.Edited().ramKB == 6144); // 4096 -> 6 MB (Classic II sizes)
	m.HandleKey(hid::Right);
	m.HandleKey(hid::Right);
	CHECK(m.Edited().ramKB == 2048); // wraps from 10 MB to the Classic II's smallest
	m.HandleKey(hid::Right);
	m.HandleKey(hid::Right);
	m.HandleKey(hid::Right); // 10 MB
	m.HandleKey(hid::Up);
	m.HandleKey(hid::Right); // ROM to the Classic: 10 MB is shown as 4 MB
	CHECK(StrEqual(m.Edited().rom, "Classic.rom"));
	m.HandleKey(hid::Down);
	m.HandleKey(hid::Left);
	CHECK(m.Edited().ramKB == 2048); // 4 MB (fitted) -> 2 MB

	m.HandleKey(hid::Down); // disk
	m.HandleKey(hid::Right);
	CHECK(StrEqual(m.Edited().disk, "System7.vhd"));
	m.HandleKey(hid::Right);
	CHECK(m.Edited().disk[0] == '\0');

	m.HandleKey(hid::Down); // floppy: Left/Right choose, Enter acts
	CHECK(m.Selected() == SettingsMenu::ItemFloppy);
	m.HandleKey(hid::Left);
	CHECK(StrEqual(m.Edited().floppy[0], "Disk B.dsk"));
	CHECK(m.HandleKey(hid::Enter) == MenuResult::ChangeFloppy && m.ChangedDrive() == 0);
	m.HandleKey(hid::Right);
	CHECK(m.Edited().floppy[0][0] == '\0');
	CHECK(m.HandleKey(hid::Enter) == MenuResult::None); // empty, unchanged: picks one
	CHECK(StrEqual(m.Edited().floppy[0], "Disk A.dsk"));

	m.HandleKey(hid::Down); // external floppy
	CHECK(m.Selected() == SettingsMenu::ItemFloppyExternal);
	m.HandleKey(hid::Right);
	CHECK(StrEqual(m.Edited().floppy[1], "Disk A.dsk"));
	CHECK(m.HandleKey(hid::Enter) == MenuResult::ChangeFloppy && m.ChangedDrive() == 1);
	m.SetFloppyInDrive(1, "Disk A.dsk");
	CHECK(StrEqual(m.Edited().floppy[1], "Disk A.dsk"));

	m.HandleKey(hid::Down); // CD-ROM: like a floppy drive
	CHECK(m.Selected() == SettingsMenu::ItemCdrom);
	CHECK(m.HandleKey(hid::Enter) == MenuResult::None); // empty, unchanged: picks one
	CHECK(StrEqual(m.Edited().cdrom, "Game.toast"));
	CHECK(m.HandleKey(hid::Enter) == MenuResult::ChangeCdrom);
	m.SetCdromInDrive("Game.toast");
	CHECK(m.HandleKey(hid::Enter) == MenuResult::ChangeCdrom); // unchanged: eject
	CHECK(m.Edited().cdrom[0] == '\0');
	m.SetCdromInDrive("");
	m.HandleKey(hid::Right);
	m.HandleKey(hid::Right);
	CHECK(m.Edited().cdrom[0] == '\0'); // wraps through "(empty)"

	m.HandleKey(hid::Down); // alt key
	m.HandleKey(hid::Enter);
	CHECK(!m.Edited().altIsCommand);

	m.HandleKey(hid::Down); // sound
	CHECK(m.Selected() == SettingsMenu::ItemSound);
	m.HandleKey(hid::Right);
	CHECK(m.Edited().sound == SoundJack);
	m.HandleKey(hid::Left);
	m.HandleKey(hid::Left);
	CHECK(m.Edited().sound == SoundOff);

	m.HandleKey(hid::Down); // modem port
	CHECK(m.Selected() == SettingsMenu::ItemModem);
	m.HandleKey(hid::Right);
	CHECK(!m.Edited().modem);
	m.HandleKey(hid::Down); // printer port
	m.HandleKey(hid::Enter);
	CHECK(!m.Edited().localTalk);

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
	CHECK(m.Selected() == SettingsMenu::ItemRom);
	m.HandleKey(hid::Up); // and back
	CHECK(m.Selected() == SettingsMenu::ItemReturn);
	CHECK(m.HandleKey(hid::Escape) == MenuResult::Close);
	CHECK(m.HandleKey(hid::F12) == MenuResult::Close);

	// The original settings object is untouched until applied.
	CHECK(s.altIsCommand && s.ramKB == 4096);

	// With a floppy in the drive, Enter on the unchanged choice ejects it.
	StrCopy(s.floppy[0], "Disk A.dsk", sizeof s.floppy[0]);
	m.Open(s, &roms, romInfo, &disks, &floppies);
	while (m.Selected() != SettingsMenu::ItemFloppy)
		m.HandleKey(hid::Down);
	CHECK(m.HandleKey(hid::Enter) == MenuResult::ChangeFloppy);
	CHECK(m.Edited().floppy[0][0] == '\0');
	// The external drive skips the disk that is in the internal one.
	m.HandleKey(hid::Down);
	m.HandleKey(hid::Right);
	CHECK(StrEqual(m.Edited().floppy[1], "Disk B.dsk"));
	m.HandleKey(hid::Right);
	CHECK(m.Edited().floppy[1][0] == '\0');
	m.HandleKey(hid::Left);
	m.HandleKey(hid::Left);
	CHECK(m.Edited().floppy[1][0] == '\0'); // Disk B -> (Disk A skipped) -> empty
	// Without a floppy list nothing breaks.
	m.Open(s, &roms, romInfo, &disks, 0);
	while (m.Selected() != SettingsMenu::ItemFloppy)
		m.HandleKey(hid::Down);
	m.HandleKey(hid::Right);
	CHECK(StrEqual(m.Edited().floppy[0], "Disk A.dsk"));
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

static void TestColorScaler()
{
	static u8 fb[1024 * 768];
	memset(fb, 0xEE, sizeof fb);
	Surface s = {fb, 1024, 768, 1024};
	static MonoScaler sc;
	sc.Configure(s, 512, 342);

	// 8 bpp, 512x384 (reconfigures to fill 1024x768 at 2x).
	static u8 vram[1024 * 384];
	static u32 pal[256];
	memset(vram, 0, sizeof vram);
	for (unsigned i = 0; i < 256; ++i)
		pal[i] = 0x123456; // all one colour...
	pal[0] = 0xFFFFFF;
	pal[255] = 0x000000;
	pal[5] = 0xFF0000;
	vram[0] = 5;
	vram[1] = 255;
	ColorFrame f = {vram, 512, 384, 1024, 8, pal, 1};
	CHECK(sc.Present(f) == 384);
	CHECK(sc.Scale() == 2 && sc.OffsetY() == 0);
	CHECK(sc.HostPaletteChanged());
	const u32 *host = sc.HostPalette();
	CHECK(!sc.HostPaletteChanged());
	const u8 red = fb[0], black = fb[2], white = fb[4];
	CHECK(red >= PalFirstMac && black >= PalFirstMac && white >= PalFirstMac);
	CHECK(host[red - PalFirstMac] == 0xFF0000 && host[black - PalFirstMac] == 0 &&
	      host[white - PalFirstMac] == 0xFFFFFF);
	CHECK(fb[1024 + 1] == red && fb[767 * 1024 + 1023] == white);
	CHECK(sc.Present(f) == 0);

	// A palette change that keeps the mapping only reloads the host palette.
	pal[5] = 0x00FF00;
	f.paletteSerial = 2;
	CHECK(sc.Present(f) == 0 && sc.HostPaletteChanged());
	CHECK(sc.HostPalette()[red - PalFirstMac] == 0x00FF00);

	// 256 distinct colours share 248 host entries: the nearest is reused.
	for (unsigned i = 0; i < 256; ++i)
		pal[i] = i << 16 | i << 8 | i;
	f.paletteSerial = 3;
	sc.Present(f);
	host = sc.HostPalette();
	const u8 e255 = fb[2];
	CHECK(host[e255 - PalFirstMac] == 0xF7F7F7 || host[e255 - PalFirstMac] == 0xFFFFFF ||
	      (host[e255 - PalFirstMac] & 0xFF) >= 0xF0);

	// A fade (every colour scaled, down to black and back) keeps every entry
	// on its host entry: no pixel is rewritten, only the host palette.
	const u8 e5 = fb[0];
	for (unsigned step = 0; step <= 8; ++step) {
		const unsigned level = step <= 4 ? 4 - step : step - 4; // 4..0..4
		for (unsigned i = 0; i < 256; ++i) {
			const unsigned v = i * level / 4;
			pal[i] = v << 16 | v << 8 | v;
		}
		f.paletteSerial = 10 + step;
		CHECK(sc.Present(f) == 0);
		CHECK(fb[0] == e5 && fb[2] == e255);
		host = sc.HostPalette();
		CHECK(host[e5 - PalFirstMac] == (5 * level / 4) * 0x010101u);
	}

	// A new palette that the kept mapping would show visibly wrong (unlike
	// neighbours sharing), but with few enough distinct colours to fit, is
	// remapped exactly.
	for (unsigned i = 0; i < 256; ++i) {
		pal[i] = (i % 100 * 37 & 0xFF) << 16 | (i % 100 * 91 & 0xFF) << 8 | (i % 100 * 53 & 0xFF);
		vram[i] = (u8)i;
	}
	f.paletteSerial = 30;
	CHECK(sc.Present(f) == 384);
	host = sc.HostPalette();
	bool exact = true;
	for (unsigned i = 0; i < 256; ++i)
		exact = exact && host[fb[2 * i] - PalFirstMac] == pal[i];
	CHECK(exact);
	vram[0] = 5;
	vram[1] = 255;
	for (unsigned i = 2; i < 256; ++i)
		vram[i] = 0;

	// 1 bpp through the CLUT: pixel 0 = entry 0x7F, 1 = entry 0xFF.
	pal[0x7F] = 0xFFFFFF;
	pal[0xFF] = 0x000000;
	f.depth = 1;
	f.paletteSerial = 4;
	vram[0] = 0x80;
	CHECK(sc.Present(f) == 384);
	host = sc.HostPalette();
	CHECK(host[fb[0] - PalFirstMac] == 0 && host[fb[2] - PalFirstMac] == 0xFFFFFF);

	// 16 bpp x555: pure red maps to the cube's red.
	f.depth = 16;
	vram[0] = 0x7C;
	vram[1] = 0x00;
	CHECK(sc.Present(f) == 384);
	CHECK(sc.HostPaletteChanged() && sc.HostPalette()[fb[0] - PalFirstMac] == 0xFF0000);
	CHECK(sc.HostPalette()[fb[2] - PalFirstMac] == 0x000000);

	// Back to a mono frame: full redraw with the fixed entries.
	static u8 bits[64 * 342] = {};
	MonoFrame m = {bits, 512, 342, 64};
	CHECK(sc.Present(m) == 342 && fb[42 * 1024] == PalWhite);
}

static void TestSonora()
{
	static AscV8 v8, sonora(true);
	CHECK(v8.Read(0x800) == 0xE8 && sonora.Read(0x800) == 0xBC);
	CHECK(sonora.Read(0x804) == 0x02); // A empty at reset
	for (unsigned i = 0; i < 0x3FF; ++i) {
		sonora.Write(0x000, 0x80);
		sonora.Write(0x400, 0x80);
	}
	CHECK(sonora.Read(0x804) & 0x08); // B full: the ROM's chime loop waits for this
	CHECK(!(sonora.Read(0x804) & 0x04));
	sonora.Write(0xF09, 1);
	sonora.Write(0xF29, 1);
	CHECK(!sonora.Irq());
	for (unsigned i = 0; i < 0x200; ++i)
		sonora.NextSample();
	CHECK((sonora.Read(0x804) & 0x0C) == 0x04 && !sonora.Irq()); // half empty, IRQ disabled
	sonora.Write(0xF29, 0);
	CHECK(sonora.Irq());
	CHECK(sonora.Read(0xF29) == 0 && sonora.Read(0xF09) == 1);
	sonora.Write(0x000, 0xC0); // A: +64
	sonora.Write(0x400, 0x40); // B: -64
	for (unsigned i = 0; i < 0x1FF; ++i)
		sonora.NextSample();
	CHECK(sonora.NextSample() == 0); // the mix of both channels
	sonora.Write(0x000, 0xC0);
	sonora.Write(0x400, 0xA0); // B: +32
	CHECK(sonora.NextSample() == (64 + 32) * 128);
	CHECK(sonora.NextSample() == (64 + 32) * 128); // empty: holds the last samples
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

static void TestMacClock()
{
	CHECK(MacSecondsFromDate(1904, 1, 1, 0, 0, 0) == 0);
	CHECK(MacSecondsFromDate(1904, 3, 1, 0, 0, 0) == (31u + 29u) * 86400u); // 1904 is a leap year
	CHECK(MacSecondsFromDate(2000, 1, 1, 0, 0, 0) == 3029529600u);
	CHECK(MacSecondsFromBuildStamp("Jan  1 2000", "00:00:10") == 3029529610u);
	CHECK(MacSecondsFromBuildStamp("Nov  3 2026", "12:34:56") ==
	      MacSecondsFromDate(2026, 11, 3, 12, 34, 56));
	CHECK(MacSecondsFromBuildStamp("Foo  3 2026", "12:34:56") == 0);
	CHECK(MacSecondsFromBuildStamp("Nov  3 2026", "1234:56") == 0);
}

static void TestAudio()
{
	AudioResampler r;
	r.SetRates(22254545, 48000000);

	// One Mac frame (370 samples) becomes about 798 output frames, and the
	// total over many frames matches the rate ratio closely.
	s16 in[370], out[2 * 900];
	for (unsigned i = 0; i < 370; ++i)
		in[i] = 8000;
	CHECK(r.MaxOutput(370) >= 798 && r.MaxOutput(370) < 805);
	unsigned total = 0, last = 0;
	for (unsigned f = 0; f < 601; ++f) {
		last = r.Process(in, 370, out, 900);
		total += last;
	}
	const unsigned expected = (unsigned)(601ull * 370 * 48000000 / 22254545);
	CHECK(total + 2 >= expected && total <= expected + 2);

	// A constant (DC) level decays to silence; output is stereo.
	CHECK(out[2 * (last - 1)] > -50 && out[2 * (last - 1)] < 50);
	CHECK(out[0] == out[1]);

	// A square wave keeps its pitch: count sign changes.
	r.Reset();
	unsigned crossings = 0;
	bool positive = false;
	for (unsigned f = 0; f < 60; ++f) {
		for (unsigned i = 0; i < 370; ++i)
			in[i] = ((f * 370 + i) / 37) % 2 ? 6000 : -6000; // ~300.7 Hz
		unsigned n = r.Process(in, 370, out, 900);
		for (unsigned i = 0; i < n; ++i)
			if ((out[2 * i] > 0) != positive) {
				positive = !positive;
				++crossings;
			}
	}
	// 60 frames * 370 samples / 37 = 600 half-periods.
	CHECK(crossings >= 595 && crossings <= 605);

	// Positive ppm adjustment produces more output.
	r.Reset();
	r.SetAdjustPpm(10000);
	total = 0;
	for (unsigned f = 0; f < 100; ++f)
		total += r.Process(in, 370, out, 900);
	CHECK(total > 100u * 370 * 48000 / 22254 * 1005 / 1000);
}

static void TestGcr()
{
	// 800 sectors per side over 80 tracks.
	CHECK(gcr::TrackStartBlock(80, 1) == 800 && gcr::TrackStartBlock(80, 2) == 1600);
	CHECK(gcr::SectorsPerTrack(0) == 12 && gcr::SectorsPerTrack(79) == 8);
	CHECK(gcr::TrackStartBlock(16, 2) == 384);

	for (unsigned v = 0; v < 64; ++v)
		CHECK(gcr::Decode6(gcr::Encode6((u8)v)) == (int)v);
	CHECK(gcr::Decode6(0xAA) == -1 && gcr::Decode6(0xD5) == -1);

	u8 in[524], out[524], n[gcr::kDataNibbles];
	unsigned seed = 12345;
	for (unsigned i = 0; i < 524; ++i) {
		seed = seed * 1103515245 + 12345;
		in[i] = (u8)(seed >> 16);
	}
	gcr::EncodeData(n, 7, in);
	CHECK(n[0] == 0xD5 && n[1] == 0xAA && n[2] == 0xAD && gcr::Decode6(n[3]) == 7);
	CHECK(n[707] == 0xDE && n[708] == 0xAA);
	bool allValid = true;
	for (unsigned i = 3; i < 707; ++i)
		if (gcr::Decode6(n[i]) < 0)
			allValid = false;
	CHECK(allValid);
	CHECK(gcr::DecodeData(n + 4, out) && memcmp(in, out, 524) == 0);

	// All-zero and all-0xFF sectors survive too.
	memset(in, 0, sizeof in);
	gcr::EncodeData(n, 0, in);
	CHECK(gcr::DecodeData(n + 4, out) && memcmp(in, out, 524) == 0);
	memset(in, 0xFF, sizeof in);
	gcr::EncodeData(n, 0, in);
	CHECK(gcr::DecodeData(n + 4, out) && memcmp(in, out, 524) == 0);

	// A corrupted nibble fails the checksum.
	n[300] = gcr::Encode6((u8)(gcr::Decode6(n[300]) ^ 1));
	CHECK(!gcr::DecodeData(n + 4, out));

	u8 a[gcr::kAddressNibbles];
	gcr::EncodeAddress(a, 70, 5, 1, 0x22);
	CHECK(a[0] == 0xD5 && a[2] == 0x96 && a[9] == 0xAA);
	CHECK(gcr::Decode6(a[3]) == (70 & 0x3F) && gcr::Decode6(a[4]) == 5);
	CHECK(gcr::Decode6(a[5]) == (0x20 | 1)); // side 1, track bit 6
	CHECK(gcr::Decode6(a[7]) == ((70 & 0x3F) ^ 5 ^ 0x21 ^ 0x22));
}

static void TestFloppyImage()
{
	FloppyImage img;
	CHECK(img.Attach(new u8[1000], 1000, true) == FloppyImage::TooSmall && !img.Present());
	CHECK(img.Attach(new u8[1474560], 1474560, true) == FloppyImage::Ok);
	CHECK(img.Mfm() && img.HighDensity() && img.Blocks() == 2880 && img.MfmSectorsPerTrack() == 18);
	CHECK(StrEqual(img.SizeText(), "1.44 MB"));
	CHECK(img.Attach(new u8[737280], 737280, true) == FloppyImage::Ok);
	CHECK(img.Mfm() && !img.HighDensity() && img.MfmSectorsPerTrack() == 9);
	CHECK(img.Attach(new u8[1000000], 1000000, true) == FloppyImage::UnknownSize);

	u8 *raw = new u8[819200];
	memset(raw, 0, 819200);
	CHECK(img.Attach(raw, 819200, true) == FloppyImage::Ok);
	CHECK(img.Sides() == 2 && img.Blocks() == 1600 && img.FormatByte() == 0x22 && !img.Mfm());
	u8 sec[524];
	memset(sec, 0x5A, sizeof sec);
	img.WriteSector(1599, sec);
	CHECK(img.Dirty() && img.File()[819200 - 1] == 0x5A);
	img.ReadSector(1599, sec);
	CHECK(sec[0] == 0 && sec[12] == 0x5A); // raw images have no tags

	// Disk Copy 4.2, 400K with tags.
	const unsigned size = 84 + 409600 + 800 * 12;
	u8 *dc = new u8[size];
	memset(dc, 0, size);
	dc[0] = 4;
	memcpy(dc + 1, "Test", 4);
	dc[0x40] = 0x00; dc[0x41] = 0x06; dc[0x42] = 0x40; dc[0x43] = 0x00; // 409600
	dc[0x44] = 0x00; dc[0x45] = 0x00; dc[0x46] = 0x25; dc[0x47] = 0x80; // 9600
	dc[0x50] = 0; dc[0x51] = 0x02; dc[0x52] = 0x01; dc[0x53] = 0x00;
	dc[84 + 512 * 3] = 0x77;
	dc[84 + 409600 + 12 * 3] = 0x66;
	CHECK(img.Attach(dc, size, false) == FloppyImage::Ok);
	CHECK(img.Sides() == 1 && img.Blocks() == 800 && !img.Writable() && img.FormatByte() == 0x02);
	img.ReadSector(3, sec);
	CHECK(sec[0] == 0x66 && sec[12] == 0x77);
	img.WriteSector(3, sec); // read-only: ignored
	CHECK(!img.Dirty());
	img.Detach();
}

static void TestMfm()
{
	// CRC-CCITT of the three A1 marks from FFFF.
	u16 crc = 0xFFFF;
	for (int i = 0; i < 3; ++i)
		crc = mfm::Crc(crc, 0xA1);
	CHECK(crc == mfm::kCrcAfterMarks);

	const unsigned size = 1474560;
	u8 *raw = new u8[size];
	for (unsigned i = 0; i < size; ++i)
		raw[i] = (u8)(i * 7 + i / 512);
	FloppyImage img;
	CHECK(img.Attach(raw, size, true) == FloppyImage::Ok);

	static u8 data[mfm::kMaxTrackBytes], marks[(mfm::kMaxTrackBytes + 7) / 8];
	const unsigned len = mfm::TrackBytes(true);
	mfm::BuildTrack(data, marks, len, 40, 1, img);

	// Every sector's address and data fields are there, with good CRCs.
	unsigned ids = 0, sectors = 0;
	for (unsigned i = 0; i + 520 < len; ++i) {
		if (!mfm::IsMark(marks, i) || data[i] != 0xA1 || mfm::IsMark(marks, i + 1))
			continue;
		if (data[i + 1] == 0xFE) {
			CHECK(data[i + 2] == 40 && data[i + 3] == 1 && data[i + 4] == ids + 1);
			++ids;
		} else if (data[i + 1] == 0xFB) {
			u16 c = mfm::kCrcAfterMarks;
			for (unsigned j = 1; j <= 515; ++j)
				c = mfm::Crc(c, data[i + j]);
			CHECK(c == 0);
			const unsigned block = (40 * 2 + 1) * 18 + sectors;
			CHECK(memcmp(data + i + 2, raw + block * 512, 512) == 0);
			++sectors;
		}
	}
	CHECK(ids == 18 && sectors == 18);

	// Decoding an unchanged track changes nothing.
	mfm::DecodeResult r = mfm::DecodeTrack(data, marks, len, 40, 1, img);
	CHECK(r.written == 0 && r.bad == 0 && !img.Dirty());

	// Change sector 5's data (and fix its CRC) as the Mac would write it,
	// shifted round the ring so the field wraps past the end.
	static u8 rot[mfm::kMaxTrackBytes], rotMarks[(mfm::kMaxTrackBytes + 7) / 8];
	const unsigned shift = 3000;
	for (unsigned i = 0; i < len; ++i) {
		rot[(i + len - shift) % len] = data[i];
		mfm::SetMark(rotMarks, (i + len - shift) % len, mfm::IsMark(marks, i));
	}
	bool afterId5 = false;
	for (unsigned i = 0; i < len; ++i) {
		if (!mfm::IsMark(rotMarks, i) || mfm::IsMark(rotMarks, (i + 1) % len))
			continue;
		const u8 id = rot[(i + 1) % len];
		if (id == 0xFE)
			afterId5 = rot[(i + 4) % len] == 5;
		else if (id == 0xFB && afterId5) {
			u16 c = mfm::kCrcAfterMarks;
			for (unsigned j = 1; j <= 513; ++j) {
				u8 &b = rot[(i + j) % len];
				if (j >= 2)
					b = 0x42;
				c = mfm::Crc(c, b);
			}
			rot[(i + 514) % len] = (u8)(c >> 8);
			rot[(i + 515) % len] = (u8)c;
			afterId5 = false;
		}
	}
	r = mfm::DecodeTrack(rot, rotMarks, len, 40, 1, img);
	CHECK(r.written == 1 && r.bad == 0 && img.Dirty());
	const unsigned block5 = (40 * 2 + 1) * 18 + 4;
	CHECK(img.File()[block5 * 512] == 0x42 && img.File()[block5 * 512 + 511] == 0x42);
	CHECK(img.File()[(block5 + 1) * 512] == (u8)((block5 + 1) * 512 * 7 + block5 + 1));

	// A corrupted data field is reported and not stored.
	rot[(len + 100) % len] ^= 0xFF;
	mfm::DecodeResult r2 = mfm::DecodeTrack(rot, rotMarks, len, 40, 1, img);
	CHECK(r2.written == 0);
	img.Detach();
}

// --- Serial: SCC, LocalTalk, modem -------------------------------------------

namespace {

struct FakeDatagrams : DatagramLink {
	u8 sent[8][700];
	unsigned sentLen[8];
	unsigned sentCount = 0;
	u8 queue[8][700];
	unsigned queueLen[8];
	unsigned queueHead = 0, queueCount = 0;
	void Send(const u8 *d, unsigned n) override
	{
		memcpy(sent[sentCount % 8], d, n);
		sentLen[sentCount++ % 8] = n;
	}
	unsigned Receive(u8 *d, unsigned max) override
	{
		if (queueHead == queueCount)
			return 0;
		unsigned n = queueLen[queueHead % 8];
		if (n > max)
			n = max;
		memcpy(d, queue[queueHead++ % 8], n);
		return n;
	}
	void Queue(u32 id, const u8 *frame, unsigned n)
	{
		u8 *q = queue[queueCount % 8];
		q[0] = (u8)(id >> 24);
		q[1] = (u8)(id >> 16);
		q[2] = (u8)(id >> 8);
		q[3] = (u8)id;
		memcpy(q + 4, frame, n);
		queueLen[queueCount++ % 8] = n + 4;
	}
};

struct FakeStream : StreamLink {
	State state = Idle;
	char host[64] = "";
	u16 port = 0;
	bool incoming = false;
	u8 sent[256];
	unsigned sentLen = 0;
	const u8 *in = nullptr;
	unsigned inLen = 0;
	void Connect(const char *h, u16 p) override
	{
		strcpy(host, h);
		port = p;
		state = Connecting;
	}
	bool Incoming() override { return incoming; }
	void Accept() override
	{
		incoming = false;
		state = Connected;
	}
	void Close() override
	{
		incoming = false;
		state = Idle;
	}
	State GetState() override { return state; }
	unsigned Send(const u8 *d, unsigned n) override
	{
		memcpy(sent + sentLen, d, n);
		sentLen += n;
		return n;
	}
	unsigned Receive(u8 *d, unsigned max) override
	{
		unsigned n = inLen < max ? inLen : max;
		memcpy(d, in, n);
		in += n;
		inLen -= n;
		return n;
	}
};

std::string Drain(HayesModem &m)
{
	std::string s;
	u8 c;
	while (m.RxByte(&c))
		s += (char)c;
	return s;
}

void Type(HayesModem &m, const char *text)
{
	while (*text)
		m.TxByte((u8)*text++);
}

void SccReg(Scc &scc, unsigned ch, unsigned reg, u8 v)
{
	scc.Write(ch, false, (u8)((reg & 7) | (reg >= 8 ? 0x08 : 0)), 0);
	scc.Write(ch, false, v, 0);
}

u8 SccRR(Scc &scc, unsigned ch, unsigned reg)
{
	if (reg)
		scc.Write(ch, false, (u8)reg, 0);
	return scc.Read(ch, false, 0);
}

} // namespace

static void TestLocalTalk()
{
	FakeDatagrams net;
	LocalTalkPort lt;
	lt.SetLink(&net, 0x12345678);

	// A unicast RTS stays local and is answered with a CTS from the other node.
	const u8 rts[] = {0x20, 0x10, 0x84};
	lt.SendFrame(rts, 3);
	CHECK(net.sentCount == 0);
	u8 f[700];
	CHECK(lt.ReceiveFrame(f, sizeof f) == 3 && f[0] == 0x10 && f[1] == 0x20 && f[2] == 0x85);
	CHECK(lt.ReceiveFrame(f, sizeof f) == 0);

	// Data and ENQ go out with the sender ID in front.
	const u8 data[] = {0x20, 0x10, 0x01, 0x00, 0x05, 0xAA, 0xBB};
	lt.SendFrame(data, sizeof data);
	CHECK(net.sentCount == 1 && net.sentLen[0] == 4 + sizeof data);
	CHECK(net.sent[0][0] == 0x12 && net.sent[0][3] == 0x78 && net.sent[0][4] == 0x20 &&
	      net.sent[0][10] == 0xBB);

	// Our own datagram looped back is dropped; another node's is delivered.
	net.Queue(0x12345678, data, sizeof data);
	const u8 theirs[] = {0x10, 0x20, 0x01, 0x42};
	net.Queue(0x0BADF00D, theirs, sizeof theirs);
	CHECK(lt.ReceiveFrame(f, sizeof f) == 4 && f[3] == 0x42);
	CHECK(lt.FramesReceived() == 1 && lt.FramesSent() == 1);

	// Through the SCC in SDLC mode: address search, two CRC bytes, End of Frame.
	Scc scc;
	scc.Attach(Scc::kChannelB, &lt);
	SccReg(scc, Scc::kChannelB, 4, 0x20); // SDLC
	SccReg(scc, Scc::kChannelB, 6, 0x10); // node 0x10
	SccReg(scc, Scc::kChannelB, 3, 0xDD); // Rx on, CRC, address search, hunt
	const u8 other[] = {0x33, 0x20, 0x01, 0x99};
	net.Queue(0x0BADF00D, other, sizeof other); // not for us
	net.Queue(0x0BADF00D, theirs, sizeof theirs);
	scc.Update(1000);
	CHECK(SccRR(scc, Scc::kChannelB, 0) & 0x01);
	u8 got[6];
	bool eof = false;
	for (unsigned i = 0; i < 6; ++i) {
		CHECK(SccRR(scc, Scc::kChannelB, 0) & 0x01);
		eof = (SccRR(scc, Scc::kChannelB, 1) & 0x80) != 0;
		got[i] = scc.Read(Scc::kChannelB, true, 0);
	}
	CHECK(got[0] == 0x10 && got[1] == 0x20 && got[3] == 0x42 && eof);
	CHECK(lt.NodeAddress() == 0x10);

	// Transmit: Reset Tx CRC, bytes, Reset Tx Underrun/EOM sends one frame.
	SccReg(scc, Scc::kChannelB, 5, 0x6B); // Tx on
	const unsigned before = net.sentCount;
	scc.Write(Scc::kChannelB, false, 0x80, 0);
	for (unsigned i = 0; i < sizeof data; ++i)
		scc.Write(Scc::kChannelB, true, data[i], 0);
	scc.Write(Scc::kChannelB, false, 0xC0, 0);
	CHECK(net.sentCount == before + 1 && net.sentLen[before % 8] == 4 + sizeof data);
	CHECK(SccRR(scc, Scc::kChannelB, 0) & 0x40); // Tx underrun/EOM
}

static void TestModem()
{
	FakeStream net;
	HayesModem m;
	m.SetLink(&net);
	u32 t = 0;
	m.Poll(t);

	Type(m, "AT\r");
	CHECK(Drain(m) == "AT\r\r\nOK\r\n");
	Type(m, "ATE0 S0? X4\r");
	CHECK(Drain(m) == "ATE0 S0? X4\r\r\n0\r\n\r\nOK\r\n");
	Type(m, "ATQ1\r");
	CHECK(Drain(m).empty());
	Type(m, "ATQ0 Q\r");
	CHECK(Drain(m) == "\r\nOK\r\n");
	Type(m, "ATV0\r");
	CHECK(Drain(m) == "0\r");
	Type(m, "ATV1\r");
	Drain(m);

	// Dialling
	Type(m, "ATDT bbs.example.org:2323\r");
	CHECK(net.state == StreamLink::Connecting && net.port == 2323 &&
	      strcmp(net.host, "bbs.example.org") == 0);
	CHECK(!m.Dcd());
	net.state = StreamLink::Connected;
	m.Poll(t += 20);
	CHECK(Drain(m) == "\r\nCONNECT\r\n" && m.Dcd());

	// Data both ways
	Type(m, "hello");
	m.Poll(t += 20);
	CHECK(net.sentLen == 5 && memcmp(net.sent, "hello", 5) == 0);
	static const u8 reply[] = {'h', 'i'};
	net.in = reply;
	net.inLen = 2;
	m.Poll(t += 20);
	CHECK(Drain(m) == "hi");

	// +++ with guard times, then ATH
	m.Poll(t += 1500);
	Type(m, "+++");
	m.Poll(t += 500);
	CHECK(Drain(m).empty()); // not yet: guard time after
	m.Poll(t += 1100);
	CHECK(Drain(m) == "\r\nOK\r\n" && m.Dcd());
	Type(m, "ATH\r");
	CHECK(Drain(m) == "\r\nOK\r\n" && net.state == StreamLink::Idle && !m.Dcd());

	// Telnet on port 23: options refused, IAC IAC is a data 255.
	net.sentLen = 0;
	Type(m, "ATDhost\r");
	CHECK(net.port == 23);
	net.state = StreamLink::Connected;
	m.Poll(t += 20);
	Drain(m);
	static const u8 tn[] = {255, 253, 24, 'A', 255, 255, 255, 251, 1, 'B'};
	net.in = tn;
	net.inLen = sizeof tn;
	m.Poll(t += 20);
	m.Poll(t += 20);
	const std::string got = Drain(m);
	CHECK(got.size() == 3 && got[0] == 'A' && (u8)got[1] == 255 && got[2] == 'B');
	CHECK(net.sentLen == 6 && net.sent[1] == 252 && net.sent[2] == 24 && net.sent[4] == 253 &&
	      net.sent[5] == 1);

	// The far end hangs up.
	net.state = StreamLink::Failed;
	m.Poll(t += 20);
	CHECK(Drain(m) == "\r\nNO CARRIER\r\n" && !m.Dcd());

	// Incoming call, auto-answered on the second ring.
	net.state = StreamLink::Idle;
	Type(m, "ATS0=2\r");
	Drain(m);
	net.incoming = true;
	m.Poll(t += 20);
	CHECK(Drain(m) == "\r\nRING\r\n");
	m.Poll(t += 6000);
	CHECK(Drain(m) == "\r\nRING\r\n\r\nCONNECT\r\n" && net.state == StreamLink::Connected);

	// Dropping DTR hangs up.
	m.Dtr(true);
	m.Dtr(false);
	CHECK(Drain(m) == "\r\nNO CARRIER\r\n" && net.state == StreamLink::Idle);
}

// --- SCSI CD-ROM drive -------------------------------------------------------

// A disc image in memory: every 512-byte block holds its own number.
class TestDisc : public BlockDevice {
public:
	explicit TestDisc(u32 blocks) : m_Blocks(blocks) {}
	u32 BlockCount() const override { return m_Blocks; }
	bool ReadBlocks(u32 lba, u32 count, u8 *buffer) override
	{
		for (u32 i = 0; i < count; ++i)
			for (u32 j = 0; j < kBlockSize; j += 4) {
				const u32 v = lba + i;
				buffer[i * kBlockSize + j] = (u8)(v >> 24);
				buffer[i * kBlockSize + j + 1] = (u8)(v >> 16);
				buffer[i * kBlockSize + j + 2] = (u8)(v >> 8);
				buffer[i * kBlockSize + j + 3] = (u8)v;
			}
		return true;
	}
	bool WriteBlocks(u32, u32, const u8 *) override { return false; }

private:
	u32 m_Blocks;
};

static u32 Be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

// One command, initiator side, with pseudo-DMA handshakes. Returns the status
// byte (-1: no such target); *got is the number of data-in bytes.
static int ScsiDo(Scsi5380 &s, unsigned id, const u8 *cdb, u8 *in, unsigned inCap, unsigned *got,
		  const u8 *out = 0, unsigned outLen = 0)
{
	s.Write(0, (u8)((1u << id) | 0x80), false);
	s.Write(1, 0x05, false); // SEL + data bus
	if (!(s.Read(4, false) & 0x40))
		return -1;
	s.Write(1, 0x00, false);
	unsigned ci = 0, oi = 0, n = 0;
	int status = -1;
	for (int guard = 0; guard < 1000000; ++guard) {
		const u8 bus = s.Read(4, false);
		if (!(bus & 0x40) || !(bus & 0x20))
			break; // bus free (or stuck)
		switch ((bus >> 2) & 7) {
		case 2: s.Write(0, cdb[ci++], true); break;
		case 0: s.Write(0, oi < outLen ? out[oi++] : 0, true); break;
		case 1: {
			const u8 v = s.Read(0, true);
			if (n < inCap)
				in[n] = v;
			++n;
			break;
		}
		case 3: status = s.Read(0, true); break;
		default: s.Read(0, true); break;
		}
	}
	*got = n;
	return status;
}

static void TestCdrom()
{
	static Scsi5380 s;
	static u8 buf[70000];
	unsigned got = 0;
	TestDisc disc(64 * 4); // 64 CD sectors
	s.AttachCdrom(3);

	const u8 inquiry[6] = {0x12, 0, 0, 0, 36, 0};
	CHECK(ScsiDo(s, 3, inquiry, buf, sizeof buf, &got) == 0 && got == 36);
	CHECK(buf[0] == 0x05 && (buf[1] & 0x80) && memcmp(buf + 8, "SONY    CD-ROM CDU-8003A1.9a", 28) == 0);
	CHECK(ScsiDo(s, 2, inquiry, buf, sizeof buf, &got) == -1); // nothing at ID 2

	// No disc: the drive is not ready, but Apple's mode page still answers.
	const u8 tur[6] = {0, 0, 0, 0, 0, 0};
	const u8 sense[6] = {0x03, 0, 0, 0, 18, 0};
	CHECK(ScsiDo(s, 3, tur, buf, sizeof buf, &got) == 2);
	CHECK(ScsiDo(s, 3, sense, buf, sizeof buf, &got) == 0 && buf[2] == 0x02 && buf[12] == 0x3A);
	const u8 page30[6] = {0x1A, 0, 0x30, 0, 0x24, 0};
	CHECK(ScsiDo(s, 3, page30, buf, sizeof buf, &got) == 0 && got == 36);
	CHECK(buf[12] == 0x30 && buf[13] == 22 && memcmp(buf + 14, "APPLE COMPUTER, INC   ", 22) == 0);

	// A new disc: one UNIT ATTENTION, then ready.
	s.InsertCdrom(&disc);
	CHECK(s.CdromLoaded());
	CHECK(ScsiDo(s, 3, tur, buf, sizeof buf, &got) == 2);
	CHECK(ScsiDo(s, 3, sense, buf, sizeof buf, &got) == 0 && buf[2] == 0x06 && buf[12] == 0x28);
	CHECK(ScsiDo(s, 3, tur, buf, sizeof buf, &got) == 0);

	const u8 capacity[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	CHECK(ScsiDo(s, 3, capacity, buf, sizeof buf, &got) == 0 && got == 8);
	CHECK(Be32(buf) == 63 && Be32(buf + 4) == 2048);

	// READ(10) of CD sectors 5-6: 512-byte blocks 20-27.
	const u8 read10[10] = {0x28, 0, 0, 0, 0, 5, 0, 0, 2, 0};
	CHECK(ScsiDo(s, 3, read10, buf, sizeof buf, &got) == 0 && got == 4096);
	CHECK(Be32(buf) == 20 && Be32(buf + 2048 - 4) == 23 && Be32(buf + 4096 - 4) == 27);
	const u8 readPast[10] = {0x28, 0, 0, 0, 0, 63, 0, 0, 2, 0};
	CHECK(ScsiDo(s, 3, readPast, buf, sizeof buf, &got) == 2);
	CHECK(ScsiDo(s, 3, sense, buf, sizeof buf, &got) == 0 && buf[2] == 0x05 && buf[12] == 0x21);

	// MODE SELECT to 512-byte blocks; READ(6) then counts those.
	const u8 select[6] = {0x15, 0x10, 0, 0, 12, 0};
	const u8 blockDesc[12] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x02, 0x00};
	CHECK(ScsiDo(s, 3, select, buf, sizeof buf, &got, blockDesc, 12) == 0);
	CHECK(ScsiDo(s, 3, capacity, buf, sizeof buf, &got) == 0);
	CHECK(Be32(buf) == 255 && Be32(buf + 4) == 512);
	const u8 read6[6] = {0x08, 0, 0, 9, 1, 0};
	CHECK(ScsiDo(s, 3, read6, buf, sizeof buf, &got) == 0 && got == 512 && Be32(buf) == 9);

	// Writes are refused.
	const u8 write10[10] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0};
	CHECK(ScsiDo(s, 3, write10, buf, sizeof buf, &got) == 2);
	CHECK(ScsiDo(s, 3, sense, buf, sizeof buf, &got) == 0 && buf[2] == 0x07);

	// TOC: track 1 at 0, lead-out after the last 2048-byte sector; MSF too.
	const u8 toc[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 100, 0};
	CHECK(ScsiDo(s, 3, toc, buf, sizeof buf, &got) == 0 && got == 20);
	CHECK(buf[2] == 1 && buf[3] == 1 && buf[5] == 0x14 && buf[6] == 1 && Be32(buf + 8) == 0);
	CHECK(buf[14] == 0xAA && Be32(buf + 16) == 64);
	const u8 tocMsf[10] = {0x43, 0x02, 0, 0, 0, 0, 0, 0, 100, 0};
	CHECK(ScsiDo(s, 3, tocMsf, buf, sizeof buf, &got) == 0);
	CHECK(buf[9] == 0 && buf[10] == 2 && buf[11] == 0); // 0:02:00
	CHECK(buf[17] == 0 && buf[18] == 2 && buf[19] == 64); // 64 + 150 frames
	// Multisession information (format 1 in the control byte's top bits).
	const u8 tocSession[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 12, 0x40};
	CHECK(ScsiDo(s, 3, tocSession, buf, sizeof buf, &got) == 0 && got == 12);
	CHECK(buf[1] == 10 && buf[2] == 1 && buf[3] == 1 && buf[5] == 0x14 && buf[6] == 1 &&
	      Be32(buf + 8) == 0);
	// Raw TOC (format 2): points A0 A1 A2 and track 1, MSF in BCD, as
	// Apple's driver reads the disc length from A2.
	const u8 tocRaw[10] = {0x43, 0x02, 0, 0, 0, 0, 1, 0, 0x30, 0x80};
	CHECK(ScsiDo(s, 3, tocRaw, buf, sizeof buf, &got) == 0 && got == 48);
	CHECK(buf[1] == 46 && buf[4 + 3] == 0xA0 && buf[4 + 8] == 1 && buf[15 + 3] == 0xA1 &&
	      buf[26 + 3] == 0xA2 && buf[37 + 3] == 0x01);
	CHECK(buf[26 + 8] == 0x00 && buf[26 + 9] == 0x02 && buf[26 + 10] == 0x64); // 64+150 frames = 00:02:64 BCD
	CHECK(buf[37 + 8] == 0x00 && buf[37 + 9] == 0x02 && buf[37 + 10] == 0x00);

	// Eject: refused while locked, then the Mac's eject empties the drive.
	const u8 lock[6] = {0x1E, 0, 0, 0, 1, 0};
	const u8 unlock[6] = {0x1E, 0, 0, 0, 0, 0};
	const u8 eject[6] = {0x1B, 0, 0, 0, 0x02, 0};
	CHECK(ScsiDo(s, 3, lock, buf, sizeof buf, &got) == 0);
	CHECK(ScsiDo(s, 3, eject, buf, sizeof buf, &got) == 2 && s.CdromLoaded());
	CHECK(ScsiDo(s, 3, sense, buf, sizeof buf, &got) == 0 && buf[12] == 0x53);
	CHECK(!s.TakeCdromEjected());
	CHECK(ScsiDo(s, 3, unlock, buf, sizeof buf, &got) == 0);
	CHECK(ScsiDo(s, 3, eject, buf, sizeof buf, &got) == 0 && !s.CdromLoaded());
	CHECK(s.TakeCdromEjected() && !s.TakeCdromEjected());
	CHECK(ScsiDo(s, 3, tur, buf, sizeof buf, &got) == 2);

	// Reinserted after a reset: 2048-byte blocks again.
	s.Reset();
	s.InsertCdrom(&disc);
	CHECK(ScsiDo(s, 3, tur, buf, sizeof buf, &got) == 2); // unit attention
	CHECK(ScsiDo(s, 3, capacity, buf, sizeof buf, &got) == 0 && Be32(buf + 4) == 2048);
}

int main()
{
	TestKeymap();
	TestKeyboardTracker();
	TestSettings();
	TestMenu();
	TestRomAndVhd();
	TestScaler();
	TestColorScaler();
	TestSonora();
	TestTestMachine();
	TestTextPage();
	TestMacClock();
	TestAudio();
	TestGcr();
	TestFloppyImage();
	TestMfm();
	TestLocalTalk();
	TestModem();
	TestCdrom();
	printf("%d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
