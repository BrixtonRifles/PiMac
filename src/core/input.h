// Host input events. USB handlers run in interrupt context and push into the
// queue; the main loop pops. Single producer / single consumer, one core.
#pragma once

#include "core/compat.h"

namespace pimac {

enum : u8 { EvKeyDown = 1, EvKeyUp, EvMouse };

struct InputEvent {
	u8 type;
	u8 code;    // HID usage for key events (0xE0-0xE7 for modifiers)
	u8 buttons; // mouse: bit 0 = left
	u8 reserved;
	s16 dx, dy; // mouse movement
};

class InputQueue {
public:
	static const unsigned kSize = 256; // power of two

	InputQueue() : m_Head(0), m_Tail(0) {}
	bool Push(const InputEvent &e);
	bool Pop(InputEvent &e);
	void Clear() { m_Tail = m_Head; }

private:
	InputEvent m_Events[kSize];
	volatile unsigned m_Head; // written by producer
	volatile unsigned m_Tail; // written by consumer
};

// Turns USB boot-protocol keyboard reports (modifier byte + up to six key
// usages) into individual key down/up events.
class KeyboardTracker {
public:
	KeyboardTracker();
	void Update(u8 modifiers, const u8 keys[6], InputQueue &q);
	void ReleaseAll(InputQueue &q);

private:
	u8 m_Modifiers;
	u8 m_Keys[6];
};

void PushMouse(InputQueue &q, unsigned buttons, int dx, int dy);

} // namespace pimac
