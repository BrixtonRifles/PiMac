#include "core/text_page.h"
#include "core/draw.h"
#include "core/keymap.h"

namespace pimac {

namespace {

const int kMargin = 32;   // outside the panel
const int kPad = 20;      // inside the panel
const int kTitleH = 48;
const int kFooterH = 28;
const int kRowH = 12;

const u8 kHidPageUp = 0x4B;
const u8 kHidPageDown = 0x4E;
const u8 kHidHome = 0x4A;
const u8 kHidEnd = 0x4D;

} // namespace

TextPage::TextPage() : m_Count(0), m_Top(0), m_LastVisible(40)
{
}

void TextPage::AddLine(const char *text)
{
	if (m_Count >= kMaxLines) {
		// Keep the newest lines.
		memcpy(m_Lines[0], m_Lines[1], (kMaxLines - 1) * kLineLen);
		m_Count = kMaxLines - 1;
	}
	StrCopy(m_Lines[m_Count++], text, kLineLen);
}

void TextPage::AddText(const char *text, unsigned len)
{
	char line[kLineLen];
	unsigned n = 0;
	for (unsigned i = 0; i < len; ++i) {
		char c = text[i];
		if (c == '\x1b') {
			// Skip an ANSI escape sequence: ESC [ ... final letter
			++i;
			while (i < len && !((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z')))
				++i;
			continue;
		}
		if (c == '\r')
			continue;
		if (c == '\n') {
			line[n] = '\0';
			AddLine(line);
			n = 0;
			continue;
		}
		if (c == '\t')
			c = ' ';
		if ((u8)c < 0x20 || (u8)c > 0x7E)
			c = '?';
		if (n + 1 < kLineLen)
			line[n++] = c;
	}
	if (n) {
		line[n] = '\0';
		AddLine(line);
	}
}

void TextPage::ClampTop(unsigned visible)
{
	unsigned maxTop = m_Count > visible ? m_Count - visible : 0;
	if (m_Top > maxTop)
		m_Top = maxTop;
}

bool TextPage::HandleKey(u8 usage)
{
	unsigned page = m_LastVisible > 1 ? m_LastVisible - 1 : 1;
	switch (usage) {
	case hid::Escape:
	case hid::F12:
	case hid::Enter:
	case hid::KeypadEnter:
		return true;
	case hid::Up:
		if (m_Top)
			--m_Top;
		break;
	case hid::Down:
		++m_Top;
		break;
	case kHidPageUp:
		m_Top = m_Top > page ? m_Top - page : 0;
		break;
	case kHidPageDown:
		m_Top += page;
		break;
	case kHidHome:
		m_Top = 0;
		break;
	case kHidEnd:
		m_Top = m_Count;
		break;
	default:
		break;
	}
	ClampTop(m_LastVisible);
	return false;
}

unsigned TextPage::VisibleRows(const Surface &s)
{
	int h = (int)s.height - 2 * kMargin - kTitleH - kFooterH - kPad;
	return h > kRowH ? (unsigned)(h / kRowH) : 1;
}

void TextPage::Render(Surface &s, const char *title) const
{
	const int x0 = kMargin, y0 = kMargin;
	const int w = (int)s.width - 2 * kMargin, h = (int)s.height - 2 * kMargin;
	const unsigned visible = VisibleRows(s);
	const unsigned maxChars = (unsigned)((w - 2 * kPad) / 8);

	// Render is const but remembers the page size for PgUp/PgDn.
	TextPage *self = const_cast<TextPage *>(this);
	self->m_LastVisible = visible;
	self->ClampTop(visible);

	FillRect(s, x0, y0, w, h, PalPanel);
	DrawFrame(s, x0, y0, w, h, PalPanelText, 2);
	DrawText(s, x0 + kPad, y0 + 16, title, PalPanelText, -1, 2);
	FillRect(s, x0 + kPad, y0 + kTitleH - 6, w - 2 * kPad, 2, PalPanelDim);

	char buf[kLineLen];
	for (unsigned r = 0; r < visible && m_Top + r < m_Count; ++r) {
		StrCopy(buf, m_Lines[m_Top + r], sizeof buf);
		if (strlen(buf) > maxChars)
			buf[maxChars] = '\0';
		DrawText(s, x0 + kPad, y0 + kTitleH + 4 + (int)r * kRowH, buf, PalPanelText, -1, 1);
	}

	char footer[96];
	StrCopy(footer, "Up/Down/PgUp/PgDn/Home/End scroll   Esc back      lines ", sizeof footer);
	StrAppendUInt(footer, m_Count ? m_Top + 1 : 0, sizeof footer);
	StrAppend(footer, "-", sizeof footer);
	StrAppendUInt(footer, m_Top + visible < m_Count ? m_Top + visible : m_Count, sizeof footer);
	StrAppend(footer, " of ", sizeof footer);
	StrAppendUInt(footer, m_Count, sizeof footer);
	DrawText(s, x0 + kPad, y0 + h - kFooterH + 8, footer, PalPanelDim, -1, 1);
}

} // namespace pimac
