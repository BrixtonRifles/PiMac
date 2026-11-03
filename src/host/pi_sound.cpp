#include "host/pi_sound.h"

#include <circle/logger.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/timer.h>

#include "core/settings.h"

static const char FromSound[] = "sound";

static const unsigned kSampleRate = 48000;
static const unsigned kQueueMs = 120;
static const unsigned kBufferFrames = 2048; // > one emulated frame (~800)
// DMA chunk sizes in samples (both channels). Small chunks keep the queue
// level smooth, which the rate control relies on.
static const unsigned kHdmiChunk = 384 * 4; // must be a multiple of 384
static const unsigned kPwmChunk = 1024;

PiSound::PiSound(CInterruptSystem *pInterrupt)
	: m_pInterrupt(pInterrupt), m_pDevice(0), m_nOutput(pimac::SoundOff), m_nQueueFrames(0),
	  m_nFillAverage(0), m_nAdjustPpm(0), m_nRefills(0), m_pBuffer(new s16[2 * kBufferFrames])
{
}

PiSound::~PiSound()
{
	Stop();
	delete[] m_pBuffer;
}

const char *PiSound::DeviceName() const
{
	if (!m_pDevice)
		return "off";
	return m_nOutput == pimac::SoundJack ? "headphone jack" : "HDMI";
}

void PiSound::Stop()
{
	if (!m_pDevice)
		return;
	m_pDevice->Cancel();
	for (unsigned i = 0; i < 100 && m_pDevice->IsActive(); ++i)
		CTimer::Get()->MsDelay(1);
	delete m_pDevice;
	m_pDevice = 0;
}

bool PiSound::Start(unsigned nOutput)
{
	Stop();
	m_nOutput = nOutput;
	if (nOutput == pimac::SoundOff)
		return true;

	if (nOutput == pimac::SoundJack)
		m_pDevice = new CPWMSoundBaseDevice(m_pInterrupt, kSampleRate, kPwmChunk);
	else
		m_pDevice = new CHDMISoundBaseDevice(m_pInterrupt, kSampleRate, kHdmiChunk);

	if (!m_pDevice->AllocateQueue(kQueueMs)) {
		CLogger::Get()->Write(FromSound, LogError, "cannot allocate sound queue");
		delete m_pDevice;
		m_pDevice = 0;
		return false;
	}
	m_pDevice->SetWriteFormat(SoundFormatSigned16, 2);
	m_nQueueFrames = m_pDevice->GetQueueSizeFrames();

	m_Resampler.Reset();
	m_nAdjustPpm = 0;
	m_nFillAverage = (m_nQueueFrames / 2) * 16;
	WriteSilence(m_nQueueFrames / 2);

	if (!m_pDevice->Start()) {
		CLogger::Get()->Write(FromSound, LogError, "cannot start %s sound", DeviceName());
		delete m_pDevice;
		m_pDevice = 0;
		return false;
	}
	CLogger::Get()->Write(FromSound, LogNotice, "sound on %s, %u Hz", DeviceName(), kSampleRate);
	return true;
}

void PiSound::WriteSilence(unsigned nFrames)
{
	memset(m_pBuffer, 0, 2 * kBufferFrames * sizeof(s16));
	while (nFrames > 0) {
		unsigned n = nFrames < kBufferFrames ? nFrames : kBufferFrames;
		m_pDevice->Write(m_pBuffer, n * 2 * sizeof(s16));
		nFrames -= n;
	}
}

unsigned PiSound::QueuedMs()
{
	return m_pDevice ? m_pDevice->GetQueueFramesAvail() * 1000 / kSampleRate : 0;
}

void PiSound::Feed(const s16 *pMono, unsigned nCount, u32 nRateMilliHz)
{
	if (!m_pDevice || nCount == 0)
		return;

	const unsigned nTarget = m_nQueueFrames / 2;
	unsigned nQueued = m_pDevice->GetQueueFramesAvail();

	// After a pause (settings menu, slow SD access) the queue has drained:
	// pad it back to the target instead of crackling while it refills.
	if (nQueued < m_nQueueFrames / 8) {
		WriteSilence(nTarget - nQueued);
		nQueued = nTarget;
		m_nFillAverage = nTarget * 16;
		++m_nRefills;
	}
	// Far too full (the emulator ran ahead): drop this frame.
	if (nQueued > m_nQueueFrames * 7 / 8)
		return;

	// Proportional control on the smoothed fill level: +-2000 ppm at the
	// ends of the queue, far below anything audible as a pitch change.
	m_nFillAverage = m_nFillAverage - m_nFillAverage / 16 + nQueued;
	const int nError = (int)nTarget - (int)(m_nFillAverage / 16);
	m_nAdjustPpm = nError * 2000 / (int)nTarget;
	m_Resampler.SetAdjustPpm(m_nAdjustPpm);

	m_Resampler.SetRates(nRateMilliHz, kSampleRate * 1000);
	unsigned nFrames = m_Resampler.Process(pMono, nCount, m_pBuffer, kBufferFrames);
	m_pDevice->Write(m_pBuffer, nFrames * 2 * sizeof(s16));
}
