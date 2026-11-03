// Converts the emulated machine's mono sample stream to the host's stereo
// output rate. Linear interpolation, with a DC-blocking filter in front (Mac
// software often leaves the sound buffer at a constant level that is not
// mid-scale, which would otherwise click on every change).
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
	void UpdateStep();

	u32 m_InMilliHz;
	u32 m_OutMilliHz;
	int m_AdjustPpm;
	u64 m_Step;  // input samples per output frame, 32.32 fixed point
	u64 m_Phase; // position between m_Prev and the next input sample, 32.32
	s32 m_Prev;
	s32 m_DcIn;  // DC blocker state
	s32 m_DcOut;
};

} // namespace pimac
