#include "host/pi_display.h"

#include <circle/logger.h>

static const char FromDisplay[] = "display";

PiDisplay::PiDisplay() : m_pFrameBuffer(0)
{
	m_Surface.pixels = 0;
	m_Surface.width = m_Surface.height = m_Surface.pitch = 0;
}

PiDisplay::~PiDisplay()
{
	delete m_pFrameBuffer;
}

bool PiDisplay::Initialize(unsigned width, unsigned height)
{
	m_pFrameBuffer = new CBcmFrameBuffer(width, height, 8);

	for (unsigned i = 0; i < pimac::PalCount; ++i)
		m_pFrameBuffer->SetPalette((u8)i, pimac::RGB565(pimac::kPaletteRGB[i]));

	if (!m_pFrameBuffer->Initialize()) {
		CLogger::Get()->Write(FromDisplay, LogError, "Cannot allocate %ux%u framebuffer",
				      width, height);
		return false;
	}

	m_Surface.pixels = (u8 *)(uintptr)m_pFrameBuffer->GetBuffer();
	m_Surface.width = m_pFrameBuffer->GetWidth();
	m_Surface.height = m_pFrameBuffer->GetHeight();
	m_Surface.pitch = m_pFrameBuffer->GetPitch();

	pimac::FillRect(m_Surface, 0, 0, (int)m_Surface.width, (int)m_Surface.height,
			pimac::PalBorder);

	CLogger::Get()->Write(FromDisplay, LogNotice, "Framebuffer %ux%u, pitch %u",
			      m_Surface.width, m_Surface.height, m_Surface.pitch);
	return true;
}
