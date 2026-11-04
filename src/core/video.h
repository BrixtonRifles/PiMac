// Host framebuffer abstraction and the Mac screen -> 8-bit scaler.
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
	PalCount,
	// Entries PalFirstMac..255 belong to colour Mac screens (see
	// MonoScaler::HostPalette).
	PalFirstMac = 8,
	PalMacCount = 256 - PalFirstMac
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

// An indexed-colour screen (the Color Classic's VRAM): 1, 2, 4 or 8 bits
// per pixel through `palette` (256 x 0xRRGGBB, the Mac's CLUT; an n-bit
// pixel p uses entry (p << (8-n)) | (0xFF >> n), as Ariel does), or 16 bits
// x555 big-endian (palette unused). paletteSerial changes with the palette.
struct ColorFrame {
	const u8 *bits;
	unsigned width;
	unsigned height;
	unsigned rowBytes;
	unsigned depth;
	const u32 *palette;
	u32 paletteSerial;
};

void FillRect(Surface &s, int x, int y, int w, int h, u8 color);

// Draws a MonoFrame or ColorFrame centred on the surface at the largest
// whole-number scale that fits (512x342 on 1024x768 -> 2x, 42px black bars
// top and bottom; 512x384 fills it). A frame of another size reconfigures it.
// Only rows that changed since the previous frame are redrawn.
//
// Colour frames go through host palette entries PalFirstMac..255: indexed
// depths give each CLUT entry one of them (at 8 bpp, 256 entries into 248:
// exact duplicates share, and past that the nearest); when only the colours
// change (fades, flashes) entries keep their host entries, so just the host
// palette is reloaded and no pixel is rewritten. 16 bpp uses a
// fixed 6x6x6 cube plus 32 greys. When HostPaletteChanged() the host must
// load HostPalette() (PalMacCount x 0xRRGGBB) into entries PalFirstMac on.
class MonoScaler {
public:
	static const unsigned kMaxSrcBytes = 1024 * 384; // 512x384 at 16 bpp
	static const unsigned kMaxLine = 4096;

	MonoScaler();

	void Configure(const Surface &dst, unsigned srcWidth, unsigned srcHeight);
	void Invalidate() { m_ForceFull = true; }

	// Returns the number of source rows redrawn.
	unsigned Present(const MonoFrame &src);
	unsigned Present(const ColorFrame &src);

	bool HostPaletteChanged() const { return m_HostPalChanged; }
	const u32 *HostPalette() { m_HostPalChanged = false; return m_HostPal; }

	unsigned Scale() const { return m_Scale; }
	unsigned OffsetX() const { return m_OffX; }
	unsigned OffsetY() const { return m_OffY; }

private:
	void DrawBorders();
	bool Fit(unsigned width, unsigned height);
	void MapPalette(const ColorFrame &src, bool keep);
	void FreshMap(const ColorFrame &src);
	unsigned UpdateMap(const ColorFrame &src);
	static unsigned Nearest(const u32 *pal, unsigned count, u32 rgb);
	void Build16Palette();

	Surface m_Dst;
	unsigned m_SrcW, m_SrcH;
	unsigned m_Scale, m_OffX, m_OffY;
	bool m_ForceFull;
	u8 m_Prev[kMaxSrcBytes];
	u8 m_Line[kMaxLine];

	// Colour state
	bool m_Color;           // last frame was a ColorFrame
	unsigned m_Depth;       // its depth
	u32 m_PalSerial;        // its palette serial
	u8 m_Map[256];          // Mac CLUT index -> host index
	u32 m_HostPal[PalMacCount];
	static const u16 kNoOwner = 0xFFFF;
	u16 m_Owner[PalMacCount]; // CLUT entry that sets each host entry's colour
	bool m_HostPalChanged;
	bool m_Have16;          // m_HostPal holds the 16 bpp palette
	u8 m_Lut16[32768];      // x555 -> host index
};

} // namespace pimac
