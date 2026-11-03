#include "core/settings.h"

namespace pimac {

const MachineModel kMachineModels[] = {
	{"classic", "Macintosh Classic"},
	{"plus", "Macintosh Plus"},
	{"se", "Macintosh SE"},
};
const unsigned kMachineModelCount = sizeof kMachineModels / sizeof kMachineModels[0];

const unsigned kRamOptionsKB[] = {1024, 2048, 2560, 4096};
const unsigned kRamOptionCount = sizeof kRamOptionsKB / sizeof kRamOptionsKB[0];

const char *MachineName(const char *id)
{
	for (unsigned i = 0; i < kMachineModelCount; ++i)
		if (StrEqual(kMachineModels[i].id, id))
			return kMachineModels[i].name;
	return "Unknown";
}

void SettingsDefaults(Settings &s)
{
	StrCopy(s.machine, "classic", sizeof s.machine);
	s.ramKB = 4096;
	s.rom[0] = '\0';
	s.disk[0] = '\0';
	s.altIsCommand = true;
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

		if (StrEqual(key, "machine")) {
			for (unsigned m = 0; m < kMachineModelCount; ++m)
				if (StrEqual(kMachineModels[m].id, value))
					StrCopy(s.machine, value, sizeof s.machine);
		} else if (StrEqual(key, "ram_kb")) {
			unsigned v;
			if (ParseUInt(value, v))
				for (unsigned r = 0; r < kRamOptionCount; ++r)
					if (kRamOptionsKB[r] == v)
						s.ramKB = v;
		} else if (StrEqual(key, "rom")) {
			StrCopy(s.rom, value, sizeof s.rom);
		} else if (StrEqual(key, "disk")) {
			StrCopy(s.disk, value, sizeof s.disk);
		} else if (StrEqual(key, "alt_is_command")) {
			s.altIsCommand = ParseBool(value);
		}
	}
}

unsigned SettingsSerialize(const Settings &s, char *out, unsigned cap)
{
	if (cap == 0)
		return 0;
	out[0] = '\0';
	StrAppend(out, "# PiMac settings (edited by the F12 menu)\n", cap);
	StrAppend(out, "machine=", cap);
	StrAppend(out, s.machine, cap);
	StrAppend(out, "\nram_kb=", cap);
	StrAppendUInt(out, s.ramKB, cap);
	StrAppend(out, "\nrom=", cap);
	StrAppend(out, s.rom, cap);
	StrAppend(out, "\ndisk=", cap);
	StrAppend(out, s.disk, cap);
	StrAppend(out, "\nalt_is_command=", cap);
	StrAppend(out, s.altIsCommand ? "1" : "0", cap);
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
