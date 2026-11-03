// Renders what the HDMI screen would show to a BMP file, using the same core
// code as the Pi build: test machine at 2x, optionally with the F12 menu.
//   ./preview out.bmp [menu]
#include <stdio.h>
#include <stdlib.h>

#include "core/keymap.h"
#include "core/settings_menu.h"
#include "core/test_machine.h"
#include "core/text_page.h"
#include "core/video.h"

using namespace pimac;

static void Put32(FILE *f, u32 v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void Put16(FILE *f, u16 v) { fputc(v, f); fputc(v >> 8, f); }

static bool WriteBmp8(const char *path, const Surface &s)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	const u32 palBytes = 256 * 4, header = 14 + 40 + palBytes;
	const u32 imageBytes = s.width * s.height;
	fputc('B', f); fputc('M', f);
	Put32(f, header + imageBytes); Put32(f, 0); Put32(f, header);
	Put32(f, 40); Put32(f, s.width); Put32(f, s.height); Put16(f, 1); Put16(f, 8);
	Put32(f, 0); Put32(f, imageBytes); Put32(f, 2835); Put32(f, 2835); Put32(f, 256); Put32(f, 0);
	for (unsigned i = 0; i < 256; ++i) {
		u32 rgb = i < PalCount ? kPaletteRGB[i] : 0xFF00FF;
		fputc(rgb & 0xFF, f); fputc((rgb >> 8) & 0xFF, f); fputc((rgb >> 16) & 0xFF, f); fputc(0, f);
	}
	for (int y = (int)s.height - 1; y >= 0; --y)
		fwrite(s.pixels + y * s.pitch, 1, s.width, f);
	return fclose(f) == 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s out.bmp [menu]\n", argv[0]);
		return 2;
	}
	static u8 fb[1024 * 768];
	memset(fb, 0xFF, sizeof fb); // magenta: any pixel left undrawn shows up
	Surface s = {fb, 1024, 768, 1024};

	static TestMachine machine;
	machine.SetInfoLine(0, "ROM:  Mac Classic.rom - Macintosh Classic");
	machine.SetInfoLine(1, "Disk: System7.vhd - fixed VHD, 40 MB");
	machine.SetInfoLine(2, "Model: Macintosh Classic, 4 MB RAM");
	machine.SetInfoLine(3, "USB:  keyboard yes, mouse not detected");
	machine.MouseMotion(60, -40);
	machine.KeyEvent(0x00, true);
	machine.RunFrame();

	static MonoScaler scaler;
	scaler.Configure(s, TestMachine::kWidth, TestMachine::kHeight);
	scaler.Present(machine.Screen());

	if (argc > 2 && strcmp(argv[2], "diag") == 0) {
		static TextPage page;
		page.AddLine("Keyboard: attached (ukbd1), reports received: 12");
		page.AddLine("Mouse:    not detected, reports received: 0   (move the mouse - this updates live)");
		page.AddLine("");
		page.AddLine("Registered devices:");
		const char devs[] = "  NAME         VENDOR PRODUCT                 SERIAL\nc ukbd1        Example Keyboard\n";
		page.AddText(devs, sizeof devs - 1);
		page.AddLine("");
		page.AddLine("System log:");
		for (int i = 0; i < 60; ++i)
			page.AddLine("00:00:01.23 usbdev0-1: Interface int3-1-2 found  [sample log line for layout]");
		page.Render(s, "USB diagnostics");
	} else if (argc > 2) {
		Settings st;
		SettingsDefaults(st);
		StrCopy(st.rom, "Mac Classic.rom", sizeof st.rom);
		StrCopy(st.disk, "A very long hard disk image file name.vhd", sizeof st.disk);
		static FileList roms, disks, floppies;
		FileListAdd(roms, "Mac Classic.rom");
		FileListAdd(disks, "A very long hard disk image file name.vhd");
		FileListAdd(floppies, "System Tools.dsk");
		static SettingsMenu menu;
		menu.Open(st, &roms, &disks, &floppies);
		for (int i = 0; i < 4; ++i) // to the floppy item
			menu.HandleKey(hid::Down);
		menu.HandleKey(hid::Right);
		menu.Render(s);
	}
	return WriteBmp8(argv[1], s) ? 0 : 1;
}
