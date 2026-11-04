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
	: m_SrcW(0), m_SrcH(0), m_Scale(1), m_OffX(0), m_OffY(0), m_ForceFull(true),
	  m_Color(false), m_Depth(0), m_PalSerial(0), m_HostPalChanged(false), m_Have16(false)
{
	m_Dst.pixels = 0;
	m_Dst.width = m_Dst.height = m_Dst.pitch = 0;
	memset(m_Map, PalFirstMac, sizeof m_Map);
	memset(m_HostPal, 0, sizeof m_HostPal);
	memset(m_Owner, 0xFF, sizeof m_Owner);
}

bool MonoScaler::Fit(unsigned width, unsigned height)
{
	if (!m_Dst.pixels || !width || !height)
		return false;
	if (width != m_SrcW || height != m_SrcH)
		Configure(m_Dst, width, height);
	return true;
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
	if (!Fit(src.width, src.height))
		return 0;
	if (m_Color) {
		m_Color = false;
		m_ForceFull = true;
	}

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

// --- Colour -------------------------------------------------------------------

namespace {

unsigned Distance(u32 a, u32 b)
{
	const int dr = (int)(a >> 16 & 0xFF) - (int)(b >> 16 & 0xFF);
	const int dg = (int)(a >> 8 & 0xFF) - (int)(b >> 8 & 0xFF);
	const int db = (int)(a & 0xFF) - (int)(b & 0xFF);
	return (unsigned)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
}

} // namespace

namespace {

// The CLUT entry pixel value p uses at depth n: (p << (8 - n)) | (0xFF >> n).
unsigned ClutEntry(unsigned p, unsigned n)
{
	return ((p << (8 - n)) | (0xFFu >> n)) & 0xFF;
}

// Pixel values in mapping order: white (0) and black (last) first, as they
// matter most when entries have to share.
unsigned MapOrder(unsigned k, unsigned count)
{
	return k == 0 ? 0 : k == 1 ? count - 1 : k - 1;
}

} // namespace

// Distance() past which a shared entry's colour counts as visibly wrong:
// about 12 levels in each of red, green and blue.
static const unsigned kRemapError = 12 * 12 * 9;
// Below this (about 4 levels each) a shared entry stays where it is.
static const unsigned kKeepError = 4 * 4 * 9;

unsigned MonoScaler::Nearest(const u32 *pal, unsigned count, u32 rgb)
{
	unsigned best = 0, bestD = ~0u;
	for (unsigned i = 0; i < count; ++i) {
		const unsigned d = Distance(pal[i], rgb);
		if (d < bestD) {
			bestD = d;
			best = i;
		}
	}
	return best;
}

void MonoScaler::FreshMap(const ColorFrame &src)
{
	// Each entry gets a host entry of its own while enough remain for the
	// rest; when short, exact duplicates share, and past that the nearest.
	const unsigned n = src.depth, count = 1u << n;
	unsigned used = 0;
	for (unsigned i = 0; i < PalMacCount; ++i)
		m_Owner[i] = kNoOwner;
	for (unsigned k = 0; k < count; ++k) {
		const unsigned e = ClutEntry(MapOrder(k, count), n);
		const u32 rgb = src.palette[e] & 0xFFFFFF;
		unsigned slot = PalMacCount;
		if (count - k > PalMacCount - used)
			for (unsigned i = 0; i < used; ++i)
				if (m_HostPal[i] == rgb) {
					slot = i;
					break;
				}
		if (slot == PalMacCount) {
			if (used < PalMacCount) {
				slot = used++;
				m_HostPal[slot] = rgb;
				m_Owner[slot] = (u16)e;
			} else {
				slot = Nearest(m_HostPal, used, rgb);
			}
		}
		m_Map[e] = (u8)(PalFirstMac + slot);
	}
	for (unsigned i = used; i < PalMacCount; ++i)
		m_HostPal[i] = 0;
}

unsigned MonoScaler::UpdateMap(const ColorFrame &src)
{
	// New colours at the same depth (a fade, a flash, colour cycling): keep
	// each entry on the host entry it has, so only the host palette changes
	// and the pixels already on screen stay valid. A host entry's owner sets
	// its colour; others sharing it move only if they now differ,
	// to a free host entry (becoming its owner) or else the nearest. Owners
	// never move, so borrowing a host entry never takes it over. Returns
	// the largest Distance() between an entry and the colour shown for it.
	const unsigned n = src.depth, count = 1u << n;
	u16 *const owner = m_Owner;
	for (unsigned i = 0; i < PalMacCount; ++i)
		if (owner[i] != kNoOwner)
			m_HostPal[i] = src.palette[owner[i]] & 0xFFFFFF;
	unsigned nextFree = 0;
	unsigned worst = 0;
	for (unsigned k = 0; k < count; ++k) {
		const unsigned e = ClutEntry(MapOrder(k, count), n);
		const unsigned s = m_Map[e] - PalFirstMac;
		const u32 rgb = src.palette[e] & 0xFFFFFF;
		if (owner[s] == e || m_HostPal[s] == rgb)
			continue;
		while (nextFree < PalMacCount && owner[nextFree] != kNoOwner)
			++nextFree;
		if (nextFree < PalMacCount) {
			owner[nextFree] = (u16)e;
			m_HostPal[nextFree] = rgb;
			m_Map[e] = (u8)(PalFirstMac + nextFree);
			continue;
		}
		// Every host entry is in use: stay put while close (a fade scales
		// an entry and the one it shares with alike), else take the nearest.
		const unsigned here = Distance(m_HostPal[s], rgb);
		if (here > kKeepError) {
			const unsigned best = Nearest(m_HostPal, PalMacCount, rgb);
			if (Distance(m_HostPal[best], rgb) < here)
				m_Map[e] = (u8)(PalFirstMac + best);
		}
		const unsigned d = Distance(m_HostPal[m_Map[e] - PalFirstMac], rgb);
		if (d > worst)
			worst = d;
	}
	for (unsigned i = 0; i < PalMacCount; ++i)
		if (owner[i] == kNoOwner)
			m_HostPal[i] = 0;
	return worst;
}

void MonoScaler::MapPalette(const ColorFrame &src, bool keep)
{
	u32 old[PalMacCount];
	memcpy(old, m_HostPal, sizeof old);
	if (!keep || m_Have16) {
		FreshMap(src);
	} else if (UpdateMap(src) > kRemapError) {
		// An entry is shown visibly wrong. Start afresh (redrawing everything)
		// if that would show them all exactly: no more distinct colours than
		// host entries. Small errors are kept, as the rounding in a fade
		// would otherwise remap on every step.
		const unsigned n = src.depth, count = 1u << n;
		unsigned distinct = 0;
		for (unsigned p = 0; p < count && distinct <= PalMacCount; ++p) {
			const u32 rgb = src.palette[ClutEntry(p, n)] & 0xFFFFFF;
			bool seen = false;
			for (unsigned q = 0; q < p && !seen; ++q)
				seen = (src.palette[ClutEntry(q, n)] & 0xFFFFFF) == rgb;
			distinct += seen ? 0 : 1;
		}
		if (distinct <= PalMacCount)
			FreshMap(src);
	}
	if (memcmp(old, m_HostPal, sizeof old) != 0 || m_Have16)
		m_HostPalChanged = true;
	m_Have16 = false;
}

void MonoScaler::Build16Palette()
{
	// 6x6x6 cube, then 32 greys.
	unsigned i = 0;
	for (unsigned r = 0; r < 6; ++r)
		for (unsigned g = 0; g < 6; ++g)
			for (unsigned b = 0; b < 6; ++b)
				m_HostPal[i++] = (r * 51) << 16 | (g * 51) << 8 | (b * 51);
	for (unsigned k = 0; i < PalMacCount; ++i, ++k) {
		const unsigned v = k * 255 / 31;
		m_HostPal[i] = v << 16 | v << 8 | v;
	}
	for (unsigned c = 0; c < 32768; ++c) {
		const unsigned r = (c >> 10 & 31) * 255 / 31, g = (c >> 5 & 31) * 255 / 31,
			       b = (c & 31) * 255 / 31;
		const u32 rgb = r << 16 | g << 8 | b;
		// Nearest cube colour, or a grey if that is closer.
		const unsigned ci = ((r + 25) / 51) * 36 + ((g + 25) / 51) * 6 + (b + 25) / 51;
		const unsigned y = (r * 3 + g * 4 + b * 2) / 9;
		const unsigned gi = 216 + (y * 31 + 127) / 255;
		m_Lut16[c] = (u8)(PalFirstMac + (Distance(m_HostPal[gi], rgb) < Distance(m_HostPal[ci], rgb)
							? gi : ci));
	}
	m_Have16 = true;
	m_HostPalChanged = true;
}

unsigned MonoScaler::Present(const ColorFrame &src)
{
	const unsigned n = src.depth;
	if ((n != 1 && n != 2 && n != 4 && n != 8 && n != 16) || !Fit(src.width, src.height))
		return 0;
	const unsigned rowBytes = src.width * n / 8;
	const unsigned lineW = m_SrcW * m_Scale;
	if (rowBytes * m_SrcH > kMaxSrcBytes || lineW > kMaxLine || (n < 16 && !src.palette))
		return 0;

	if (!m_Color || n != m_Depth) {
		m_Color = true;
		m_Depth = n;
		m_ForceFull = true;
		if (n == 16 && !m_Have16)
			Build16Palette();
		else if (n < 16)
			MapPalette(src, false);
		m_PalSerial = src.paletteSerial;
	} else if (n < 16 && src.paletteSerial != m_PalSerial) {
		m_PalSerial = src.paletteSerial;
		u8 before[256];
		memcpy(before, m_Map, sizeof before);
		MapPalette(src, true);
		if (memcmp(before, m_Map, sizeof before) != 0)
			m_ForceFull = true; // pixels now need other host entries
	}

	unsigned outW = lineW;
	if (m_OffX + outW > m_Dst.width)
		outW = m_Dst.width - m_OffX;
	const bool full = m_ForceFull;
	m_ForceFull = false;
	if (full)
		DrawBorders();

	const unsigned shift = 8 - n, mask = (1u << n) - 1, low = 0xFFu >> n;
	unsigned redrawn = 0;
	for (unsigned y = 0; y < m_SrcH; ++y) {
		const u8 *row = src.bits + y * src.rowBytes;
		u8 *prev = m_Prev + y * rowBytes;
		if (!full && memcmp(row, prev, rowBytes) == 0)
			continue;
		memcpy(prev, row, rowBytes);

		u8 *o = m_Line;
		if (n == 16) {
			for (unsigned x = 0; x < m_SrcW; ++x) {
				const u8 c = m_Lut16[((unsigned)row[2 * x] << 8 | row[2 * x + 1]) & 0x7FFF];
				for (unsigned k = 0; k < m_Scale; ++k)
					*o++ = c;
			}
		} else {
			for (unsigned bx = 0; bx < rowBytes; ++bx) {
				const unsigned b = row[bx];
				for (int s = 8 - (int)n; s >= 0; s -= (int)n) {
					const u8 c = m_Map[(((b >> s) & mask) << shift | low) & 0xFF];
					for (unsigned k = 0; k < m_Scale; ++k)
						*o++ = c;
				}
			}
		}
		for (unsigned k = 0; k < m_Scale; ++k) {
			const unsigned dy = m_OffY + y * m_Scale + k;
			if (dy >= m_Dst.height)
				break;
			memcpy(m_Dst.pixels + dy * m_Dst.pitch + m_OffX, m_Line, outW);
		}
		++redrawn;
	}
	return redrawn;
}

} // namespace pimac
