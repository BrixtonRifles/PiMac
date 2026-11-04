// Persistent settings (SD:/pimac.cfg, simple key=value text).
#pragma once

#include "core/compat.h"

namespace pimac {

static const unsigned kFloppyDrives = 2;

struct Settings {
	unsigned ramKB;
	unsigned cpuPercent; // CPU speed: one of kCpuSpeeds (Classic II / Color Classic)
	char rom[64];      // file name inside /roms ("" = first found); picks the model
	char disk[64];     // file name inside /disks ("" = none)
	// File names inside /floppies ("" = drive empty), by drive: internal,
	// external.
	char floppy[kFloppyDrives][64];
	char cdrom[64];    // file name inside /cdroms ("" = no disc in the CD drive)
	bool altIsCommand; // PC Alt key acts as Command
	unsigned sound;    // SoundOutput
	// Serial ports, carried over the Pi's network.
	bool modem;          // modem port: Hayes modem that dials TCP (telnet)
	unsigned modemListen; // TCP port that rings the modem (0 = none)
	bool localTalk;      // printer port: LocalTalk over UDP (LToUDP)
};

enum SoundOutput { SoundHdmi, SoundJack, SoundOff, SoundOutputCount };
const char *SoundOutputName(unsigned output); // for the menu

// CPU speeds the menu offers, in percent of the real Mac. Faster costs the
// Pi proportionally more CPU (the Mac never idles).
extern const unsigned kCpuSpeeds[];
extern const unsigned kCpuSpeedCount;

// Every RAM size any model supports (core/rom.h has each model's own list).
extern const unsigned kRamOptionsKB[];
extern const unsigned kRamOptionCount;

void SettingsDefaults(Settings &s);
// Applies recognised keys from text; unknown keys and bad values are ignored.
void SettingsParse(Settings &s, const char *text, unsigned len);
// Returns bytes written (excluding the terminating NUL).
unsigned SettingsSerialize(const Settings &s, char *out, unsigned cap);

// A sorted list of file names found on the SD card.
struct FileList {
	enum { kMax = 32, kNameLen = 64 };
	char names[kMax][kNameLen];
	unsigned count;
};

void FileListClear(FileList &l);
void FileListAdd(FileList &l, const char *name);
int FileListFind(const FileList &l, const char *name); // -1 if absent

} // namespace pimac
