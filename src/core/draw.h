// Text and rectangle drawing for 8-bit surfaces (overlay UI) and
// 1-bit canvases (emulated / test Mac screens).
#pragma once

#include "core/video.h"

namespace pimac {

// --- 8-bit palettised surface ---------------------------------------------
// bg < 0 means transparent background.
void DrawChar(Surface &s, int x, int y, char ch, u8 fg, int bg, unsigned scale);
int DrawText(Surface &s, int x, int y, const char *text, u8 fg, int bg, unsigned scale);
void DrawFrame(Surface &s, int x, int y, int w, int h, u8 color, int thickness);

// --- 1-bit canvas (MSB first, 1 = black) ----------------------------------
struct MonoCanvas {
	u8 *bits;
	unsigned width;
	unsigned height;
	unsigned rowBytes;
};

void MonoPixel(MonoCanvas &c, int x, int y, bool black);
void MonoFill(MonoCanvas &c, int x, int y, int w, int h, bool black);
void MonoFillPattern(MonoCanvas &c, int x, int y, int w, int h, const u8 pattern[8]);
void MonoRect(MonoCanvas &c, int x, int y, int w, int h, bool black);
int MonoText(MonoCanvas &c, int x, int y, const char *text, bool black);

} // namespace pimac
