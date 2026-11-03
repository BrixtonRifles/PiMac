#include "core/test_machine.h"
#include "core/draw.h"

namespace pimac {

namespace {

const u8 kDesktopGray[8] = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};

// Classic Mac arrow cursor, hotspot (1,1).
const u16 kArrowData[16] = {
	0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
	0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000,
};
const u16 kArrowMask[16] = {
	0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80,
	0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780, 0x0380,
};

const int kWinX = 24, kWinY = 52, kWinW = 464, kWinH = 236;
const unsigned kMaxTextChars = (kWinW - 24) / 8;

void DrawWindowText(MonoCanvas &c, int x, int y, const char *text)
{
	char clipped[kMaxTextChars + 1];
	StrCopy(clipped, text, sizeof clipped);
	MonoText(c, x, y, clipped, true);
}

} // namespace

TestMachine::TestMachine()
{
	for (unsigned i = 0; i < kInfoLines; ++i)
		m_Info[i][0] = '\0';
	Reset();
}

void TestMachine::SetInfoLine(unsigned i, const char *text)
{
	if (i < kInfoLines)
		StrCopy(m_Info[i], text, kLineLen);
}

void TestMachine::Reset()
{
	m_MouseX = kWidth / 2;
	m_MouseY = kHeight / 2;
	m_Button = false;
	m_LastKey = 0xFF;
	m_LastKeyDown = false;
	m_Frames = 0;
	Redraw();
}

void TestMachine::RunFrame()
{
	++m_Frames;
	Redraw();
}

MonoFrame TestMachine::Screen() const
{
	MonoFrame f;
	f.bits = m_Bits;
	f.width = kWidth;
	f.height = kHeight;
	f.rowBytes = kRowBytes;
	return f;
}

void TestMachine::KeyEvent(u8 macKey, bool down)
{
	m_LastKey = macKey;
	m_LastKeyDown = down;
}

void TestMachine::MouseMotion(int dx, int dy)
{
	m_MouseX += dx;
	m_MouseY += dy;
	if (m_MouseX < 0) m_MouseX = 0;
	if (m_MouseY < 0) m_MouseY = 0;
	if (m_MouseX >= kWidth) m_MouseX = kWidth - 1;
	if (m_MouseY >= kHeight) m_MouseY = kHeight - 1;
}

void TestMachine::MouseButton(bool down)
{
	m_Button = down;
}

void TestMachine::ReleaseAllInputs()
{
	m_Button = false;
	m_LastKeyDown = false;
}

void TestMachine::Redraw()
{
	MonoCanvas c = {m_Bits, kWidth, kHeight, kRowBytes};

	// Desktop and menu bar
	MonoFillPattern(c, 0, 0, kWidth, kHeight, kDesktopGray);
	MonoFill(c, 0, 0, kWidth, 19, false);
	MonoFill(c, 0, 19, kWidth, 1, true);
	MonoText(c, 14, 6, "File  Edit  View  Special", true);

	// Window with drop shadow, striped title bar and title
	MonoFill(c, kWinX, kWinY, kWinW, kWinH, false);
	MonoRect(c, kWinX, kWinY, kWinW, kWinH, true);
	MonoFill(c, kWinX + 1, kWinY + kWinH, kWinW, 1, true);
	MonoFill(c, kWinX + kWinW, kWinY + 1, 1, kWinH, true);
	for (int y = kWinY + 4; y < kWinY + 16; y += 2)
		MonoFill(c, kWinX + 2, y, kWinW - 4, 1, true);
	MonoFill(c, kWinX, kWinY + 18, kWinW, 1, true);
	const char *title = "PiMac";
	int tw = 8 * 5 + 16;
	MonoFill(c, kWinX + (kWinW - tw) / 2, kWinY + 2, tw, 15, false);
	MonoText(c, kWinX + (kWinW - tw) / 2 + 8, kWinY + 6, title, true);

	int x = kWinX + 12, y = kWinY + 32;
	const int lh = 16;
	DrawWindowText(c, x, y, "Bare-metal test screen - no 68000 core yet.");
	y += lh * 2;
	for (unsigned i = 0; i < kInfoLines; ++i, y += lh)
		DrawWindowText(c, x, y, m_Info[i]);
	y += lh;

	char line[kLineLen];
	StrCopy(line, "Last key: ", sizeof line);
	if (m_LastKey == 0xFF) {
		StrAppend(line, "-", sizeof line);
	} else {
		StrAppend(line, "$", sizeof line);
		StrAppendHex(line, m_LastKey, 2, sizeof line);
		StrAppend(line, m_LastKeyDown ? " down" : " up", sizeof line);
	}
	DrawWindowText(c, x, y, line);
	y += lh;

	StrCopy(line, "Mouse: ", sizeof line);
	StrAppendUInt(line, (unsigned)m_MouseX, sizeof line);
	StrAppend(line, ",", sizeof line);
	StrAppendUInt(line, (unsigned)m_MouseY, sizeof line);
	StrAppend(line, m_Button ? "  button down" : "  button up", sizeof line);
	DrawWindowText(c, x, y, line);
	y += lh;

	StrCopy(line, "Uptime: ", sizeof line);
	StrAppendUInt(line, m_Frames / 60, sizeof line);
	StrAppend(line, " s", sizeof line);
	DrawWindowText(c, x, y, line);
	y += lh * 2;

	DrawWindowText(c, x, y, "Press F12 for settings.");

	// Cursor
	for (int r = 0; r < 16; ++r)
		for (int col = 0; col < 16; ++col) {
			u16 bit = (u16)(0x8000 >> col);
			if (kArrowMask[r] & bit)
				MonoPixel(c, m_MouseX - 1 + col, m_MouseY - 1 + r, (kArrowData[r] & bit) != 0);
		}
}

} // namespace pimac
