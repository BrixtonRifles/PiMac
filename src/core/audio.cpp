#include "core/audio.h"

namespace pimac {

static const u64 kOne = (u64)1 << 32;

// 4th-order Butterworth low-pass, 8 kHz cutoff, designed for the 48 kHz
// output (both hosts use 48 kHz; at another rate the cutoff scales with it).
// Two RBJ sections with Q = 0.5412 and 1.3066, coefficients in Q28. At
// 11-24 kHz this takes the interpolation images 20-30 dB further down; up to
// 5 kHz it changes the level by less than 0.2 dB.
static const s32 kLowPass[2][4] = {
	// b0 (= b2), b1, a1, a2
	{ 37280566, 74561132, -149122264, 29809071 },
	{ 50404221, 100808442, -201616884, 134798311 },
};

AudioResampler::AudioResampler()
	: m_InMilliHz(22254545), m_OutMilliHz(48000000), m_AdjustPpm(0)
{
	UpdateStep();
	Reset();
}

void AudioResampler::Reset()
{
	m_Phase = 0;
	m_Prev = 0;
	m_DcIn = 0;
	m_DcOut = 0;
	for (unsigned s = 0; s < 2; ++s) {
		Biquad &q = m_LowPass[s];
		q.b0 = kLowPass[s][0];
		q.b1 = kLowPass[s][1];
		q.a1 = kLowPass[s][2];
		q.a2 = kLowPass[s][3];
		q.x1 = q.x2 = q.y1 = q.y2 = 0;
	}
}

void AudioResampler::SetRates(u32 inMilliHz, u32 outMilliHz)
{
	if (inMilliHz == m_InMilliHz && outMilliHz == m_OutMilliHz)
		return;
	m_InMilliHz = inMilliHz;
	m_OutMilliHz = outMilliHz;
	UpdateStep();
}

void AudioResampler::SetAdjustPpm(int ppm)
{
	if (ppm > 20000)
		ppm = 20000;
	if (ppm < -20000)
		ppm = -20000;
	if (ppm == m_AdjustPpm)
		return;
	m_AdjustPpm = ppm;
	UpdateStep();
}

void AudioResampler::UpdateStep()
{
	// step = in / (out * (1 + ppm/1e6)), computed as a 32.32 fraction.
	const u64 out = (u64)m_OutMilliHz * (u64)(1000000 + m_AdjustPpm) / 1000000;
	m_Step = out ? ((u64)m_InMilliHz << 32) / out : kOne;
	if (m_Step == 0)
		m_Step = 1;
}

unsigned AudioResampler::MaxOutput(unsigned inCount) const
{
	return (unsigned)(((u64)inCount << 32) / m_Step) + 2;
}

unsigned AudioResampler::Process(const s16 *in, unsigned inCount, s16 *out, unsigned maxFrames)
{
	unsigned produced = 0;
	for (unsigned i = 0; i < inCount; ++i) {
		// DC blocker: y = x - x[-1] + 0.995 * y[-1] (about 18 Hz at 22 kHz).
		const s32 x = in[i];
		s32 y = x - m_DcIn + (s32)(((s64)m_DcOut * 32604) >> 15);
		m_DcIn = x;
		if (y > 32767)
			y = 32767;
		if (y < -32768)
			y = -32768;
		m_DcOut = y;

		// Emit every output frame that falls between m_Prev and y.
		while (m_Phase < kOne) {
			if (produced == maxFrames) {
				m_Phase = kOne; // out of room: drop the rest of this input
				break;
			}
			const s32 frac = (s32)(m_Phase >> 17); // 15 bits
			s32 v = m_Prev + (s32)(((s64)(y - m_Prev) * frac) >> 15);
			v = m_LowPass[1].Filter(m_LowPass[0].Filter(v));
			if (v > 32767) // the filter overshoots a little on full-scale steps
				v = 32767;
			if (v < -32768)
				v = -32768;
			out[2 * produced] = (s16)v;
			out[2 * produced + 1] = (s16)v;
			++produced;
			m_Phase += m_Step;
		}
		m_Phase -= kOne;
		m_Prev = y;
	}
	return produced;
}

} // namespace pimac
