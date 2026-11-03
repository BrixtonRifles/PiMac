// Host framebuffer abstraction and the 1-bit Mac screen -> 8-bit scaler.
#pragma once

#include "core/compat.h"

namespace pimac {

// An 8-bit palettised drawing surface (the HDMI framebuffer on the Pi).
struct Surface {
	u8 *pixels;
	unsigned width;
	unsigned height;
	unsigned pitch; // bytes per row
};

// Palette layout shared by the emulated screen and the settings overlay.
enum : u8 {
	PalBlack = 0,
	PalWhite,
	PalBorder,
	PalPanel,
	PalPanelText,
	PalPanelDim,
	PalHighlight,
	PalHighlightText,
	PalCount
};

extern const u32 kPaletteRGB[PalCount]; // 0xRRGGBB

inline u16 RGB565(u32 rgb)
{
	return (u16)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

// A 1 bit-per-pixel screen as the compact Macs store it:
// MSB = leftmost pixel, 1 = black.
struct MonoFrame {
	const u8 *bits;
	unsigned width; // multiple of 8
	unsigned height;
	unsigned rowBytes;
};

void FillRect(Surface &s, int x, int y, int w, int h, u8 color);

// Draws a MonoFrame centred on the surface at the largest whole-number scale
// that fits (512x342 on 1024x768 -> 2x, 42px black bars top and bottom).
// Only rows that changed since the previous frame are redrawn.
class MonoScaler {
public:
	static const unsigned kMaxSrcBytes = 640 / 8 * 480;
	static const unsigned kMaxLine = 4096;

	MonoScaler();

	void Configure(const Surface &dst, unsigned srcWidth, unsigned srcHeight);
	void Invalidate() { m_ForceFull = true; }

	// Returns the number of source rows redrawn.
	unsigned Present(const MonoFrame &src);

	unsigned Scale() const { return m_Scale; }
	unsigned OffsetX() const { return m_OffX; }
	unsigned OffsetY() const { return m_OffY; }

private:
	void DrawBorders();

	Surface m_Dst;
	unsigned m_SrcW, m_SrcH;
	unsigned m_Scale, m_OffX, m_OffY;
	bool m_ForceFull;
	u8 m_Prev[kMaxSrcBytes];
	u8 m_Line[kMaxLine];
};

} // namespace pimac
