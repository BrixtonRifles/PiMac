#include "core/audio.h"

namespace pimac {

static const u64 kOne = (u64)1 << 32;

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
			const s16 v = (s16)(m_Prev + (s32)(((s64)(y - m_Prev) * frac) >> 15));
			out[2 * produced] = v;
			out[2 * produced + 1] = v;
			++produced;
			m_Phase += m_Step;
		}
		m_Phase -= kOne;
		m_Prev = y;
	}
	return produced;
}

} // namespace pimac
