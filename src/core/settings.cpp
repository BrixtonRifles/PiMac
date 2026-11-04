#include "core/settings.h"

namespace pimac {

// Each model uses the nearest size it supports (Classic 1-4 MB, Classic II 2-10 MB).
const unsigned kRamOptionsKB[] = {1024, 2048, 4096, 6144, 8192, 10240};
const unsigned kRamOptionCount = sizeof kRamOptionsKB / sizeof kRamOptionsKB[0];

static const char *const kSoundIds[SoundOutputCount] = {"hdmi", "jack", "off"};
static const char *const kSoundNames[SoundOutputCount] = {"HDMI", "Headphone jack", "Off"};

const char *SoundOutputName(unsigned output)
{
	return output < SoundOutputCount ? kSoundNames[output] : "?";
}

void SettingsDefaults(Settings &s)
{
	s.ramKB = 4096;
	s.rom[0] = '\0';
	s.disk[0] = '\0';
	for (unsigned d = 0; d < kFloppyDrives; ++d)
		s.floppy[d][0] = '\0';
	s.cdrom[0] = '\0';
	s.altIsCommand = true;
	s.sound = SoundHdmi;
	s.modem = true;
	s.modemListen = 0;
	s.localTalk = true;
}

static bool IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

static char *Trim(char *s)
{
	while (IsSpace(*s))
		++s;
	unsigned n = (unsigned)strlen(s);
	while (n && IsSpace(s[n - 1]))
		s[--n] = '\0';
	return s;
}

static bool ParseUInt(const char *s, unsigned &out)
{
	if (!*s)
		return false;
	unsigned v = 0;
	for (; *s; ++s) {
		if (*s < '0' || *s > '9')
			return false;
		v = v * 10 + (unsigned)(*s - '0');
	}
	out = v;
	return true;
}

static bool ParseBool(const char *s)
{
	return StrEqual(s, "1") || StrEqual(s, "yes") || StrEqual(s, "true") || StrEqual(s, "on");
}

void SettingsParse(Settings &s, const char *text, unsigned len)
{
	unsigned i = 0;
	while (i < len) {
		unsigned start = i;
		while (i < len && text[i] != '\n')
			++i;
		unsigned n = i - start;
		if (i < len)
			++i;

		char line[160];
		if (n >= sizeof line)
			n = sizeof line - 1;
		memcpy(line, text + start, n);
		line[n] = '\0';

		char *key = Trim(line);
		if (!*key || *key == '#')
			continue;
		char *eq = key;
		while (*eq && *eq != '=')
			++eq;
		if (!*eq)
			continue;
		*eq = '\0';
		key = Trim(key);
		char *value = Trim(eq + 1);

		// "machine" (older versions) is ignored: the ROM picks the model.
		if (StrEqual(key, "ram_kb")) {
			unsigned v;
			if (ParseUInt(value, v))
				for (unsigned r = 0; r < kRamOptionCount; ++r)
					if (kRamOptionsKB[r] == v)
						s.ramKB = v;
		} else if (StrEqual(key, "rom")) {
			StrCopy(s.rom, value, sizeof s.rom);
		} else if (StrEqual(key, "disk")) {
			StrCopy(s.disk, value, sizeof s.disk);
		} else if (StrEqual(key, "floppy")) {
			StrCopy(s.floppy[0], value, sizeof s.floppy[0]);
		} else if (StrEqual(key, "floppy_external")) {
			StrCopy(s.floppy[1], value, sizeof s.floppy[1]);
		} else if (StrEqual(key, "cdrom")) {
			StrCopy(s.cdrom, value, sizeof s.cdrom);
		} else if (StrEqual(key, "alt_is_command")) {
			s.altIsCommand = ParseBool(value);
		} else if (StrEqual(key, "modem")) {
			s.modem = ParseBool(value);
		} else if (StrEqual(key, "modem_listen")) {
			unsigned v;
			if (ParseUInt(value, v) && v < 65536)
				s.modemListen = v;
		} else if (StrEqual(key, "localtalk")) {
			s.localTalk = ParseBool(value);
		} else if (StrEqual(key, "sound")) {
			for (unsigned o = 0; o < SoundOutputCount; ++o)
				if (StrEqual(kSoundIds[o], value))
					s.sound = o;
		}
	}
}

unsigned SettingsSerialize(const Settings &s, char *out, unsigned cap)
{
	if (cap == 0)
		return 0;
	out[0] = '\0';
	StrAppend(out, "# PiMac settings (edited by the F12 menu)\n", cap);
	StrAppend(out, "ram_kb=", cap);
	StrAppendUInt(out, s.ramKB, cap);
	StrAppend(out, "\nrom=", cap);
	StrAppend(out, s.rom, cap);
	StrAppend(out, "\ndisk=", cap);
	StrAppend(out, s.disk, cap);
	StrAppend(out, "\nfloppy=", cap);
	StrAppend(out, s.floppy[0], cap);
	StrAppend(out, "\nfloppy_external=", cap);
	StrAppend(out, s.floppy[1], cap);
	StrAppend(out, "\ncdrom=", cap);
	StrAppend(out, s.cdrom, cap);
	StrAppend(out, "\nalt_is_command=", cap);
	StrAppend(out, s.altIsCommand ? "1" : "0", cap);
	StrAppend(out, "\nsound=", cap);
	StrAppend(out, kSoundIds[s.sound < SoundOutputCount ? s.sound : 0], cap);
	StrAppend(out, "\nmodem=", cap);
	StrAppend(out, s.modem ? "1" : "0", cap);
	StrAppend(out, "\nmodem_listen=", cap);
	StrAppendUInt(out, s.modemListen, cap);
	StrAppend(out, "\nlocaltalk=", cap);
	StrAppend(out, s.localTalk ? "1" : "0", cap);
	StrAppend(out, "\n", cap);
	return (unsigned)strlen(out);
}

void FileListClear(FileList &l)
{
	l.count = 0;
}

static int CompareNoCase(const char *a, const char *b)
{
	for (;; ++a, ++b) {
		char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
		if (ca != cb || !ca)
			return ca - cb;
	}
}

void FileListAdd(FileList &l, const char *name)
{
	if (l.count >= FileList::kMax || strlen(name) >= FileList::kNameLen)
		return;
	unsigned pos = l.count;
	while (pos > 0 && CompareNoCase(l.names[pos - 1], name) > 0) {
		memcpy(l.names[pos], l.names[pos - 1], FileList::kNameLen);
		--pos;
	}
	StrCopy(l.names[pos], name, FileList::kNameLen);
	++l.count;
}

int FileListFind(const FileList &l, const char *name)
{
	for (unsigned i = 0; i < l.count; ++i)
		if (StrEqual(l.names[i], name))
			return (int)i;
	return -1;
}

} // namespace pimac
