#include "core/video.h"

namespace pimac {

const u32 kPaletteRGB[PalCount] = {
	0x000000, // PalBlack
	0xFFFFFF, // PalWhite
	0x000000, // PalBorder
	0x24242C, // PalPanel
	0xE8E8E8, // PalPanelText
	0x8A8A96, // PalPanelDim
	0x3A6FD8, // PalHighlight
	0xFFFFFF, // PalHighlightText
};

void FillRect(Surface &s, int x, int y, int w, int h, u8 color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int)s.width)
		w = (int)s.width - x;
	if (y + h > (int)s.height)
		h = (int)s.height - y;
	if (w <= 0 || h <= 0)
		return;
	for (int r = 0; r < h; ++r)
		memset(s.pixels + (unsigned)(y + r) * s.pitch + x, color, (unsigned)w);
}

MonoScaler::MonoScaler()
	: m_SrcW(0), m_SrcH(0), m_Scale(1), m_OffX(0), m_OffY(0), m_ForceFull(true)
{
	m_Dst.pixels = 0;
	m_Dst.width = m_Dst.height = m_Dst.pitch = 0;
}

void MonoScaler::Configure(const Surface &dst, unsigned srcWidth, unsigned srcHeight)
{
	m_Dst = dst;
	m_SrcW = srcWidth;
	m_SrcH = srcHeight;

	unsigned sx = dst.width / srcWidth, sy = dst.height / srcHeight;
	m_Scale = sx < sy ? sx : sy;
	if (m_Scale == 0)
		m_Scale = 1;

	unsigned w = srcWidth * m_Scale, h = srcHeight * m_Scale;
	m_OffX = w < dst.width ? (dst.width - w) / 2 : 0;
	m_OffY = h < dst.height ? (dst.height - h) / 2 : 0;
	m_ForceFull = true;
}

void MonoScaler::DrawBorders()
{
	int w = (int)(m_SrcW * m_Scale), h = (int)(m_SrcH * m_Scale);
	int dw = (int)m_Dst.width, dh = (int)m_Dst.height;
	int ox = (int)m_OffX, oy = (int)m_OffY;
	FillRect(m_Dst, 0, 0, dw, oy, PalBorder);
	FillRect(m_Dst, 0, oy + h, dw, dh - (oy + h), PalBorder);
	FillRect(m_Dst, 0, oy, ox, h, PalBorder);
	FillRect(m_Dst, ox + w, oy, dw - (ox + w), h, PalBorder);
}

unsigned MonoScaler::Present(const MonoFrame &src)
{
	if (!m_Dst.pixels || src.width != m_SrcW || src.height != m_SrcH)
		return 0;

	const unsigned rowBytes = m_SrcW / 8;
	const unsigned lineW = m_SrcW * m_Scale;
	if (rowBytes * m_SrcH > kMaxSrcBytes || lineW > kMaxLine)
		return 0;

	unsigned outW = lineW;
	if (m_OffX + outW > m_Dst.width)
		outW = m_Dst.width - m_OffX;

	const bool full = m_ForceFull;
	m_ForceFull = false;
	if (full)
		DrawBorders();

	unsigned redrawn = 0;
	for (unsigned y = 0; y < m_SrcH; ++y) {
		const u8 *row = src.bits + y * src.rowBytes;
		u8 *prev = m_Prev + y * rowBytes;
		if (!full && memcmp(row, prev, rowBytes) == 0)
			continue;
		memcpy(prev, row, rowBytes);

		u8 *o = m_Line;
		for (unsigned bx = 0; bx < rowBytes; ++bx) {
			u8 b = row[bx];
			for (unsigned bit = 0; bit < 8; ++bit, b <<= 1) {
				u8 c = (b & 0x80) ? PalBlack : PalWhite;
				for (unsigned k = 0; k < m_Scale; ++k)
					*o++ = c;
			}
		}

		for (unsigned k = 0; k < m_Scale; ++k) {
			unsigned dy = m_OffY + y * m_Scale + k;
			if (dy >= m_Dst.height)
				break;
			memcpy(m_Dst.pixels + dy * m_Dst.pitch + m_OffX, m_Line, outW);
		}
		++redrawn;
	}
	return redrawn;
}

} // namespace pimac
