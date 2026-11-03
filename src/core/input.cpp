#include "core/input.h"

namespace pimac {

bool InputQueue::Push(const InputEvent &e)
{
	unsigned head = m_Head;
	unsigned next = (head + 1) & (kSize - 1);
	if (next == m_Tail)
		return false; // full: drop
	m_Events[head] = e;
	m_Head = next;
	return true;
}

bool InputQueue::Pop(InputEvent &e)
{
	unsigned tail = m_Tail;
	if (tail == m_Head)
		return false;
	e = m_Events[tail];
	m_Tail = (tail + 1) & (kSize - 1);
	return true;
}

static void PushKey(InputQueue &q, u8 type, u8 code)
{
	InputEvent e;
	e.type = type;
	e.code = code;
	e.buttons = 0;
	e.reserved = 0;
	e.dx = e.dy = 0;
	q.Push(e);
}

static bool Contains(const u8 keys[6], u8 k)
{
	for (unsigned i = 0; i < 6; ++i)
		if (keys[i] == k)
			return true;
	return false;
}

KeyboardTracker::KeyboardTracker() : m_Modifiers(0)
{
	memset(m_Keys, 0, sizeof m_Keys);
}

void KeyboardTracker::Update(u8 modifiers, const u8 keys[6], InputQueue &q)
{
	// 0x01..0x03 are error/rollover codes: the report is not trustworthy.
	for (unsigned i = 0; i < 6; ++i)
		if (keys[i] >= 0x01 && keys[i] <= 0x03)
			return;

	u8 changed = (u8)(modifiers ^ m_Modifiers);
	for (unsigned bit = 0; bit < 8; ++bit)
		if (changed & (1u << bit))
			PushKey(q, (modifiers & (1u << bit)) ? EvKeyDown : EvKeyUp, (u8)(0xE0 + bit));

	for (unsigned i = 0; i < 6; ++i)
		if (m_Keys[i] && !Contains(keys, m_Keys[i]))
			PushKey(q, EvKeyUp, m_Keys[i]);
	for (unsigned i = 0; i < 6; ++i)
		if (keys[i] && !Contains(m_Keys, keys[i]))
			PushKey(q, EvKeyDown, keys[i]);

	m_Modifiers = modifiers;
	memcpy(m_Keys, keys, sizeof m_Keys);
}

void KeyboardTracker::ReleaseAll(InputQueue &q)
{
	static const u8 kNone[6] = {0, 0, 0, 0, 0, 0};
	Update(0, kNone, q);
}

static s16 Clamp16(int v)
{
	return (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

void PushMouse(InputQueue &q, unsigned buttons, int dx, int dy)
{
	InputEvent e;
	e.type = EvMouse;
	e.code = 0;
	e.buttons = (u8)buttons;
	e.reserved = 0;
	e.dx = Clamp16(dx);
	e.dy = Clamp16(dy);
	q.Push(e);
}

} // namespace pimac
