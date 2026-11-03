// Runs the compact-Mac emulator on the development machine, headless.
//
//   ./hostmac ROM [DISK] [options]
//     --frames N        emulate N video frames (60.15/s), default 1200
//     --shot-every K    save a screenshot every K frames (default: end only)
//     --out DIR         screenshot directory (default: out)
//     --ram KB          1024, 2048 or 4096 (default 4096)
//     --key F:CODE      press Mac key CODE (hex) at frame F; release 10 frames later
//     --hold F:CODE:N   press CODE at frame F, release after N frames
//     --mouse F:DX:DY   move the mouse at frame F
//     --click F         click the mouse button at frame F
//     --trace           print CPU state once per emulated second, PRAM at the end
//     --steps N         at the end, single-step N instructions printing registers
//
// Also writes DIR/ram.bin (a RAM dump) at the end. COWDBG=1 in the environment
// logs every block the Mac writes and how many bytes it changed.
// The disk image is never modified: writes go to an in-memory overlay.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <map>
#include <string>
#include <vector>

#include "core/blockdev.h"
#include "core/log.h"
#include "core/rom.h"
#include "mac/cpu.h"
#include "mac/mac_compact.h"

using namespace pimac;

namespace {

unsigned g_Frame = 0;

void LogToStderr(const char *line)
{
	fprintf(stderr, "[%5u] %s\n", g_Frame, line);
}

class CowDisk : public BlockDevice {
public:
	bool Open(const char *path)
	{
		m_File = fopen(path, "rb");
		if (!m_File)
			return false;
		fseeko(m_File, 0, SEEK_END);
		off_t size = ftello(m_File);
		// A fixed VHD has a 512-byte footer that is not part of the disk.
		unsigned char footer[512];
		fseeko(m_File, size - 512, SEEK_SET);
		if (fread(footer, 1, 512, m_File) == 512 && memcmp(footer, "conectix", 8) == 0)
			size -= 512;
		m_Blocks = (u32)(size / kBlockSize);
		return true;
	}

	u32 BlockCount() const override { return m_Blocks; }

	bool ReadBlocks(u32 lba, u32 count, u8 *buffer) override
	{
		for (u32 i = 0; i < count; ++i, buffer += kBlockSize) {
			auto it = m_Writes.find(lba + i);
			if (it != m_Writes.end()) {
				memcpy(buffer, it->second.data(), kBlockSize);
				continue;
			}
			fseeko(m_File, (off_t)(lba + i) * kBlockSize, SEEK_SET);
			if (fread(buffer, 1, kBlockSize, m_File) != kBlockSize)
				return false;
		}
		m_Reads += count;
		return true;
	}

	bool WriteBlocks(u32 lba, u32 count, const u8 *buffer) override
	{
		for (u32 i = 0; i < count; ++i, buffer += kBlockSize) {
			if (getenv("COWDBG")) {
				unsigned char orig[512] = {0};
				fseeko(m_File, (off_t)(lba + i) * kBlockSize, SEEK_SET);
				fread(orig, 1, 512, m_File);
				fprintf(stderr, "write %u:", lba + i);
				for (int j = 0; j < 16; ++j) fprintf(stderr, " %02X", buffer[j]);
				int nd = 0, first = -1, last = -1;
				for (int j = 0; j < 512; ++j)
					if (orig[j] != buffer[j]) { ++nd; if (first < 0) first = j; last = j; }
				fprintf(stderr, "  [%d bytes differ, %d..%d]\n", nd, first, last);
			}
			m_Writes[lba + i].assign(buffer, buffer + kBlockSize);
		}
		return true;
	}

	size_t BlocksWritten() const { return m_Writes.size(); }

private:
	FILE *m_File = nullptr;
	u32 m_Blocks = 0;
	u32 m_Reads = 0;
	std::map<u32, std::vector<u8>> m_Writes;
};

bool ReadFile(const char *path, std::vector<u8> &out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	out.resize((size_t)ftell(f));
	fseek(f, 0, SEEK_SET);
	bool ok = fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	return ok;
}

void Put32(FILE *f, u32 v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
void Put16(FILE *f, u16 v) { fputc(v, f); fputc(v >> 8, f); }

void SaveScreen(const MonoFrame &s, const std::string &path)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return;
	const u32 pal = 2 * 4, header = 14 + 40 + pal;
	const u32 rowBytes = ((s.width + 31) / 32) * 4;
	fputc('B', f); fputc('M', f);
	Put32(f, header + rowBytes * s.height); Put32(f, 0); Put32(f, header);
	Put32(f, 40); Put32(f, s.width); Put32(f, s.height); Put16(f, 1); Put16(f, 1);
	Put32(f, 0); Put32(f, rowBytes * s.height); Put32(f, 2835); Put32(f, 2835); Put32(f, 2); Put32(f, 0);
	Put32(f, 0x00FFFFFF); // index 0: white (Mac bit 0)
	Put32(f, 0x00000000); // index 1: black (Mac bit 1)
	std::vector<u8> row(rowBytes, 0);
	for (int y = (int)s.height - 1; y >= 0; --y) {
		memcpy(row.data(), s.bits + y * s.rowBytes, s.width / 8);
		fwrite(row.data(), 1, rowBytes, f);
	}
	fclose(f);
}

struct Action {
	unsigned frame;
	enum { KeyDown, KeyUp, Mouse, ButtonDown, ButtonUp } kind;
	int a, b;
};

u32 MacClockNow()
{
	time_t t = time(nullptr);
	struct tm lt;
	localtime_r(&t, &lt);
	return (u32)((long long)t + lt.tm_gmtoff + 2082844800LL);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s ROM [DISK] [options] (see source)\n", argv[0]);
		return 2;
	}

	const char *romPath = argv[1];
	const char *diskPath = nullptr;
	unsigned frames = 1200, shotEvery = 0, ramKB = 4096;
	std::string outDir = "out";
	bool trace = false;
	unsigned steps = 0;
	std::vector<Action> actions;

	for (int i = 2; i < argc; ++i) {
		std::string arg = argv[i];
		auto next = [&]() -> const char * {
			if (i + 1 >= argc) {
				fprintf(stderr, "missing value for %s\n", arg.c_str());
				exit(2);
			}
			return argv[++i];
		};
		if (arg == "--frames") frames = (unsigned)atoi(next());
		else if (arg == "--shot-every") shotEvery = (unsigned)atoi(next());
		else if (arg == "--out") outDir = next();
		else if (arg == "--ram") ramKB = (unsigned)atoi(next());
		else if (arg == "--trace") trace = true;
		else if (arg == "--steps" && i + 1 < argc) steps = (unsigned)atoi(argv[++i]);
		else if (arg == "--key" || arg == "--hold") {
			unsigned f, code, n = 10;
			const char *v = next();
			if (arg == "--key" ? sscanf(v, "%u:%x", &f, &code) != 2
					   : sscanf(v, "%u:%x:%u", &f, &code, &n) != 3) {
				fprintf(stderr, "bad %s value %s\n", arg.c_str(), v);
				return 2;
			}
			actions.push_back({f, Action::KeyDown, (int)code, 0});
			actions.push_back({f + n, Action::KeyUp, (int)code, 0});
		} else if (arg == "--mouse") {
			unsigned f; int dx, dy;
			if (sscanf(next(), "%u:%d:%d", &f, &dx, &dy) != 3) return 2;
			actions.push_back({f, Action::Mouse, dx, dy});
		} else if (arg == "--click") {
			unsigned f = (unsigned)atoi(next());
			actions.push_back({f, Action::ButtonDown, 0, 0});
			actions.push_back({f + 6, Action::ButtonUp, 0, 0});
		} else if (arg[0] != '-' && !diskPath) diskPath = argv[i];
		else {
			fprintf(stderr, "unknown option %s\n", arg.c_str());
			return 2;
		}
	}

	SetLogHandler(LogToStderr);

	std::vector<u8> rom;
	if (!ReadFile(romPath, rom)) {
		fprintf(stderr, "cannot read ROM %s\n", romPath);
		return 1;
	}
	RomInfo info = IdentifyRom(rom.data(), (unsigned)rom.size());
	fprintf(stderr, "ROM: %s (%08X), %zu bytes\n", info.model, info.checksum, rom.size());

	static CowDisk disk;
	bool haveDisk = false;
	if (diskPath) {
		if (!disk.Open(diskPath)) {
			fprintf(stderr, "cannot open disk %s\n", diskPath);
			return 1;
		}
		haveDisk = true;
		fprintf(stderr, "Disk: %s, %u blocks (%u MB)\n", diskPath, disk.BlockCount(),
			disk.BlockCount() / 2048);
	}

	static MacCompact mac;
	MacConfig cfg;
	cfg.ramKB = ramKB;
	cfg.rom = rom.data();
	cfg.romSize = (unsigned)rom.size();
	cfg.disk = haveDisk ? &disk : nullptr;
	cfg.clockSeconds = MacClockNow();
	if (!mac.Start(cfg)) {
		fprintf(stderr, "start failed: %s\n", mac.Error());
		return 1;
	}

	mkdir(outDir.c_str(), 0755);
	const clock_t t0 = clock();

	for (g_Frame = 0; g_Frame < frames; ++g_Frame) {
		for (const Action &a : actions) {
			if (a.frame != g_Frame)
				continue;
			switch (a.kind) {
			case Action::KeyDown: mac.KeyEvent((u8)a.a, true); break;
			case Action::KeyUp: mac.KeyEvent((u8)a.a, false); break;
			case Action::Mouse: mac.MouseMotion(a.a, a.b); break;
			case Action::ButtonDown: mac.MouseButton(true); break;
			case Action::ButtonUp: mac.MouseButton(false); break;
			}
		}

		mac.RunFrame();

		if (trace && g_Frame % 60 == 0)
			fprintf(stderr,
				"[%5u] PC=%06X SR=%04X SP=%06X D0=%08X A0=%06X overlay=%d scsi r/w=%u/%u\n",
				g_Frame, cpu::PC(), cpu::SR(), cpu::SP(), cpu::D(0), cpu::A(0),
				mac.Overlay() ? 1 : 0, mac.GetScsi().BlocksRead(),
				mac.GetScsi().BlocksWritten());

		if (shotEvery && g_Frame % shotEvery == 0) {
			char name[64];
			snprintf(name, sizeof name, "/frame_%05u.bmp", g_Frame);
			SaveScreen(mac.Screen(), outDir + name);
		}
	}

	for (unsigned i = 0; i < steps; ++i) {
		fprintf(stderr, "step PC=%06X SR=%04X D0=%08X D1=%08X A0=%06X A1=%06X SP=%06X\n", cpu::PC(),
			cpu::SR(), cpu::D(0), cpu::D(1), cpu::A(0), cpu::A(1), cpu::SP());
		cpu::Execute(1);
	}
	if (FILE *f = fopen((outDir + "/ram.bin").c_str(), "wb")) { // for offline disassembly
		fwrite(mac.Ram(), 1, mac.RamSize(), f);
		fclose(f);
	}
	if (trace) {
		fprintf(stderr, "RTC seconds %08X\n", mac.GetRtc().Seconds());
		fprintf(stderr, "PRAM:");
		for (unsigned i = 0; i < 256; ++i)
			fprintf(stderr, "%s%02X", i % 32 ? " " : "\n  ", mac.GetRtc().Pram()[i]);
		fprintf(stderr, "\n");
	}
	SaveScreen(mac.Screen(), outDir + "/final.bmp");
	const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
	fprintf(stderr, "Ran %u frames (%.1f emulated s) in %.2f s host CPU: %.1fx real time\n",
		frames, frames / 60.15, secs, secs > 0 ? frames / 60.15 / secs : 0.0);
	fprintf(stderr, "SCSI blocks read %u, written %u (kept in memory)\n",
		mac.GetScsi().BlocksRead(), mac.GetScsi().BlocksWritten());
	return 0;
}
