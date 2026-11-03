// A scrollable full-screen text page (used for USB diagnostics / logs).
#pragma once

#include "core/video.h"

namespace pimac {

class TextPage {
public:
	enum { kMaxLines = 200, kLineLen = 116 };

	TextPage();

	// Clear() keeps the scroll position, so a page can be rebuilt live.
	void Clear() { m_Count = 0; }
	void AddLine(const char *text);
	// Appends text that may contain several '\n'-separated lines and ANSI
	// escape sequences (stripped). Tabs become spaces.
	void AddText(const char *text, unsigned len);

	// Returns true when the page should close (Esc, F12, Enter).
	bool HandleKey(u8 hidUsage);
	void Render(Surface &s, const char *title) const;

	unsigned LineCount() const { return m_Count; }
	const char *Line(unsigned i) const { return m_Lines[i]; }
	unsigned Top() const { return m_Top; }

	static unsigned VisibleRows(const Surface &s);

private:
	void ClampTop(unsigned visible);

	char m_Lines[kMaxLines][kLineLen];
	unsigned m_Count;
	unsigned m_Top;
	unsigned m_LastVisible;
};

} // namespace pimac
