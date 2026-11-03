// USB keyboard and mouse via Circle. The Circle handlers run in interrupt
// context; they only translate reports and push into an InputQueue.
#pragma once

#include <circle/devicenameservice.h>
#include <circle/input/mouse.h>
#include <circle/usb/usbkeyboard.h>
#include "core/input.h"

class PiInput {
public:
	explicit PiInput(CDeviceNameService *pDeviceNameService);

	// Call after CUSBHCIDevice::UpdatePlugAndPlay() reports a change.
	void UpdateDevices();

	pimac::InputQueue &Queue() { return m_Queue; }

private:
	static void KeyStatusHandlerRaw(unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void MouseStatusHandler(unsigned nButtons, int nDisplacementX, int nDisplacementY,
				       int nWheelMove);
	static void KeyboardRemovedHandler(CDevice *pDevice, void *pContext);
	static void MouseRemovedHandler(CDevice *pDevice, void *pContext);

	CDeviceNameService *m_pDeviceNameService;
	CUSBKeyboardDevice *volatile m_pKeyboard;
	CMouseDevice *volatile m_pMouse;
	pimac::InputQueue m_Queue;
	pimac::KeyboardTracker m_Tracker;

	static PiInput *s_pThis;
};
