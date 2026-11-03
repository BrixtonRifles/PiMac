// Stand-in "machine" used until a real 68000 core is integrated. Draws a
// Mac-style desktop at 512x342 and echoes keyboard/mouse input, which
// exercises the display, scaling, input and settings paths end to end.
#pragma once

#include "core/machine.h"

namespace pimac {

class TestMachine : public Machine {
public:
	enum { kWidth = 512, kHeight = 342, kRowBytes = kWidth / 8, kInfoLines = 4, kLineLen = 64 };

	TestMachine();

	// Free-form information lines shown in the window (ROM, disk, ...).
	void SetInfoLine(unsigned i, const char *text);

	void Reset() override;
	void RunFrame() override;
	MonoFrame Screen() const override;
	void KeyEvent(u8 macKey, bool down) override;
	void MouseMotion(int dx, int dy) override;
	void MouseButton(bool down) override;
	void ReleaseAllInputs() override;

private:
	void Redraw();

	u8 m_Bits[kRowBytes * kHeight];
	char m_Info[kInfoLines][kLineLen];
	int m_MouseX, m_MouseY;
	bool m_Button;
	u8 m_LastKey;
	bool m_LastKeyDown;
	unsigned m_Frames;
};

} // namespace pimac
