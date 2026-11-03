// Sound output on the Pi: HDMI audio or the 3.5 mm headphone jack (PWM),
// both driven directly by Circle (no GPU firmware audio).
//
// Feed() takes one emulated frame of mono samples, resamples it to 48 kHz
// and queues it. The resampling ratio is trimmed to keep the queue about
// half full, so the emulator's timer-paced clock and the audio hardware's
// clock never drift apart audibly.
#pragma once

#include <circle/interrupt.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/types.h>

#include "core/audio.h"

class PiSound {
public:
	explicit PiSound(CInterruptSystem *pInterrupt);
	~PiSound();

	// output: pimac::SoundOutput. Stops any running device first.
	bool Start(unsigned nOutput);
	void Stop();
	bool Active() const { return m_pDevice != 0; }
	const char *DeviceName() const;

	void Feed(const s16 *pMono, unsigned nCount, u32 nRateMilliHz);

	// Diagnostics
	unsigned QueuedMs();
	int AdjustPpm() const { return m_nAdjustPpm; }
	unsigned Refills() const { return m_nRefills; }

private:
	void WriteSilence(unsigned nFrames);

	CInterruptSystem *m_pInterrupt;
	CSoundBaseDevice *m_pDevice;
	unsigned m_nOutput;
	unsigned m_nQueueFrames;
	unsigned m_nFillAverage; // queued frames, smoothed (x16)
	int m_nAdjustPpm;
	unsigned m_nRefills;     // times the queue ran low and was padded
	pimac::AudioResampler m_Resampler;
	s16 *m_pBuffer;          // stereo output frames for one Feed()
};
