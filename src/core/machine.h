// Interface every emulated machine implements. The kernel owns the frame
// loop; a machine runs one video frame of emulated time per RunFrame().
#pragma once

#include "core/video.h"

namespace pimac {

class Machine {
public:
	virtual void Reset() = 0;
	virtual void RunFrame() = 0;
	virtual MonoFrame Screen() const = 0;

	// macKey is a Mac virtual key code (see keymap.h).
	virtual void KeyEvent(u8 macKey, bool down) = 0;
	virtual void MouseMotion(int dx, int dy) = 0;
	virtual void MouseButton(bool down) = 0;
	// Called when focus moves to the settings menu, so no key stays stuck.
	virtual void ReleaseAllInputs() = 0;

	// Sound produced during the last RunFrame(): mono samples, written to
	// `out` (at most `max`). Returns the count; `rateMilliHz` gets the rate.
	virtual unsigned SoundFrame(s16 *out, unsigned max, u32 &rateMilliHz)
	{
		(void)out;
		(void)max;
		rateMilliHz = 0;
		return 0;
	}

protected:
	~Machine() {}
};

} // namespace pimac
