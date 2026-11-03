#include "core/draw.h"
#include "core/font8x8.h"

namespace pimac {

static const u8 *Glyph(char ch)
{
	unsigned idx = (u8)ch;
	if (idx < 0x20 || idx > 0x7F)
		idx = '?';
	return kFont8x8[idx - 0x20];
}

void DrawChar(Surface &s, int x, int y, char ch, u8 fg, int bg, unsigned scale)
{
	const u8 *g = Glyph(ch);
	for (int r = 0; r < 8; ++r)
		for (int c = 0; c < 8; ++c) {
			bool on = (g[r] >> c) & 1;
			if (!on && bg < 0)
				continue;
			FillRect(s, x + c * (int)scale, y + r * (int)scale, (int)scale, (int)scale,
				 on ? fg : (u8)bg);
		}
}

int DrawText(Surface &s, int x, int y, const char *text, u8 fg, int bg, unsigned scale)
{
	for (; *text; ++text, x += 8 * (int)scale)
		DrawChar(s, x, y, *text, fg, bg, scale);
	return x;
}

void DrawFrame(Surface &s, int x, int y, int w, int h, u8 color, int t)
{
	FillRect(s, x, y, w, t, color);
	FillRect(s, x, y + h - t, w, t, color);
	FillRect(s, x, y, t, h, color);
	FillRect(s, x + w - t, y, t, h, color);
}

void MonoPixel(MonoCanvas &c, int x, int y, bool black)
{
	if (x < 0 || y < 0 || x >= (int)c.width || y >= (int)c.height)
		return;
	u8 &b = c.bits[(unsigned)y * c.rowBytes + ((unsigned)x >> 3)];
	u8 mask = (u8)(0x80 >> (x & 7));
	if (black)
		b |= mask;
	else
		b &= (u8)~mask;
}

void MonoFill(MonoCanvas &c, int x, int y, int w, int h, bool black)
{
	for (int j = 0; j < h; ++j)
		for (int i = 0; i < w; ++i)
			MonoPixel(c, x + i, y + j, black);
}

void MonoFillPattern(MonoCanvas &c, int x, int y, int w, int h, const u8 pattern[8])
{
	for (int j = 0; j < h; ++j)
		for (int i = 0; i < w; ++i) {
			int px = x + i, py = y + j;
			MonoPixel(c, px, py, (pattern[py & 7] >> (7 - (px & 7))) & 1);
		}
}

void MonoRect(MonoCanvas &c, int x, int y, int w, int h, bool black)
{
	MonoFill(c, x, y, w, 1, black);
	MonoFill(c, x, y + h - 1, w, 1, black);
	MonoFill(c, x, y, 1, h, black);
	MonoFill(c, x + w - 1, y, 1, h, black);
}

int MonoText(MonoCanvas &c, int x, int y, const char *text, bool black)
{
	for (; *text; ++text, x += 8) {
		const u8 *g = Glyph(*text);
		for (int r = 0; r < 8; ++r)
			for (int col = 0; col < 8; ++col)
				if ((g[r] >> col) & 1)
					MonoPixel(c, x + col, y + r, black);
	}
	return x;
}

} // namespace pimac
