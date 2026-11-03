// Persistent settings (SD:/pimac.cfg, simple key=value text).
#pragma once

#include "core/compat.h"

namespace pimac {

static const unsigned kFloppyDrives = 2;

struct Settings {
	char machine[16];  // model id, see kMachineModels
	unsigned ramKB;
	char rom[64];      // file name inside /roms ("" = first found)
	char disk[64];     // file name inside /disks ("" = none)
	// File names inside /floppies ("" = drive empty), by drive: internal,
	// external.
	char floppy[kFloppyDrives][64];
	bool altIsCommand; // PC Alt key acts as Command
	unsigned sound;    // SoundOutput
};

enum SoundOutput { SoundHdmi, SoundJack, SoundOff, SoundOutputCount };
const char *SoundOutputName(unsigned output); // for the menu

struct MachineModel {
	const char *id;
	const char *name;
};

extern const MachineModel kMachineModels[];
extern const unsigned kMachineModelCount;
extern const unsigned kRamOptionsKB[];
extern const unsigned kRamOptionCount;

const char *MachineName(const char *id);

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
