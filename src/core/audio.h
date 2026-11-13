// Converts the emulated machine's mono sample stream to the host's stereo
// output rate. Linear interpolation, with a DC-blocking filter in front (Mac
// software often leaves the sound buffer at a constant level that is not
// mid-scale, which would otherwise click on every change), and a low-pass
// filter after it: the Macs' 22 kHz output went through an analog filter
// that took out the steps between samples. Without it their mirror images
// (11-24 kHz) are heard as a harsh, scratchy edge on sampled sounds.
//
// The output rate can be nudged by a few hundred ppm so the host's audio
// queue neither runs dry nor overflows: the emulator is paced by the system
// timer, the audio hardware by its own clock, and they drift apart.
#pragma once

#include "core/compat.h"

namespace pimac {

class AudioResampler {
public:
	AudioResampler();

	// Rates in millihertz (the Mac's is 22254545 mHz, not a whole number).
	void SetRates(u32 inMilliHz, u32 outMilliHz);
	// Positive ppm produces more output samples per input sample.
	void SetAdjustPpm(int ppm);
	void Reset(); // forget filter and interpolation state

	// Upper bound on frames produced from `inCount` input samples.
	unsigned MaxOutput(unsigned inCount) const;
	// Consumes `inCount` samples, writes interleaved stereo frames to `out`
	// (2 values per frame), returns the number of frames written. `maxFrames`
	// should be at least MaxOutput(inCount); any excess input is dropped.
	unsigned Process(const s16 *in, unsigned inCount, s16 *out, unsigned maxFrames);

private:
	// One second-order section, Direct Form I, Q28 coefficients (b2 = b0).
	struct Biquad {
		s32 b0, b1, a1, a2;
		s32 x1, x2, y1, y2;
		s32 Filter(s32 x)
		{
			const s64 acc = (s64)b0 * (x + x2) + (s64)b1 * x1 - (s64)a1 * y1 - (s64)a2 * y2;
			const s32 y = (s32)((acc + ((s64)1 << 27)) >> 28);
			x2 = x1;
			x1 = x;
			y2 = y1;
			y1 = y;
			return y;
		}
	};

	void UpdateStep();

	u32 m_InMilliHz;
	u32 m_OutMilliHz;
	int m_AdjustPpm;
	u64 m_Step;  // input samples per output frame, 32.32 fixed point
	u64 m_Phase; // position between m_Prev and the next input sample, 32.32
	s32 m_Prev;
	s32 m_DcIn;  // DC blocker state
	s32 m_DcOut;
	Biquad m_LowPass[2]; // 4th-order Butterworth, 8 kHz at 48 kHz output
};

} // namespace pimac
