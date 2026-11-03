// Host-side unit tests for src/core. Build and run with `make test`.
#include <stdio.h>
#include <stdlib.h>

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
	CHECK(s.floppy[0][0] == '\0' && s.floppy[1][0] == '\0');

	const char *text = "# comment\r\n  machine = se \r\nram_kb=2048\nrom=Mac Classic.rom\n"
			   "disk=System 7.vhd\nalt_is_command=0\nbogus=1\nram_kb=3000\nmachine=amiga\n"
			   "sound=jack\nsound=loud\nfloppy=System Tools.dsk\nfloppy_external=Games.img\n";
	SettingsParse(s, text, (unsigned)strlen(text));
	CHECK(StrEqual(s.machine, "se"));
	CHECK(s.ramKB == 2048);
	CHECK(StrEqual(s.rom, "Mac Classic.rom"));
	CHECK(StrEqual(s.disk, "System 7.vhd"));
	CHECK(!s.altIsCommand);
	CHECK(s.sound == SoundJack);
	CHECK(StrEqual(s.floppy[0], "System Tools.dsk") && StrEqual(s.floppy[1], "Games.img"));

	char buf[512];
	unsigned n = SettingsSerialize(s, buf, sizeof buf);
	CHECK(n == strlen(buf));
	Settings r;
	SettingsDefaults(r);
	SettingsParse(r, buf, n);
	CHECK(StrEqual(r.machine, s.machine) && r.ramKB == s.ramKB && StrEqual(r.rom, s.rom) &&
	      StrEqual(r.disk, s.disk) && r.altIsCommand == s.altIsCommand && r.sound == s.sound &&
	      StrEqual(r.floppy[0], s.floppy[0]) && StrEqual(r.floppy[1], s.floppy[1]));

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
	FileList floppies;
	FileListClear(floppies);
	FileListAdd(floppies, "Disk A.dsk");
	FileListAdd(floppies, "Disk B.dsk");

	SettingsMenu m;
	m.Open(s, &roms, &disks, &floppies);
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

	// With a floppy in the drive, Enter on the unchanged choice ejects it.
	StrCopy(s.floppy[0], "Disk A.dsk", sizeof s.floppy[0]);
	m.Open(s, &roms, &disks, &floppies);
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
	m.Open(s, &roms, &disks, 0);
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
	TestMacClock();
	TestAudio();
	TestGcr();
	TestFloppyImage();
	TestMfm();
	printf("%d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
