// HDMI framebuffer (8-bit palettised) via Circle.
#pragma once

#include <circle/bcmframebuffer.h>
#include "core/video.h"

class PiDisplay {
public:
	PiDisplay();
	~PiDisplay();

	bool Initialize(unsigned width, unsigned height);
	pimac::Surface &GetSurface() { return m_Surface; }

private:
	CBcmFrameBuffer *m_pFrameBuffer;
	pimac::Surface m_Surface;
};
