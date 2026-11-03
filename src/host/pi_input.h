// USB keyboards and mice via Circle. The Circle handlers run in interrupt
// context; they only translate reports and push into an InputQueue.
//
// Every keyboard (ukbdN) and mouse (mouseN) that Circle registers is used.
// Many keyboards also expose a mouse interface (for macro keys) and so take
// "mouse1", pushing the real mouse to "mouse2" - so never assume device 1.
#pragma once

#include <circle/devicenameservice.h>
#include <circle/input/mouse.h>
#include <circle/usb/usbkeyboard.h>
#include "core/input.h"

class PiInput {
public:
	enum { kMaxKeyboards = 4, kMaxMice = 4, kMaxDeviceNumber = 8 };

	explicit PiInput(CDeviceNameService *pDeviceNameService);

	// Call after CUSBHCIDevice::UpdatePlugAndPlay() reports a change.
	void UpdateDevices();

	pimac::InputQueue &Queue() { return m_Queue; }

	// Diagnostics
	unsigned KeyboardCount() const;
	unsigned MouseCount() const;
	unsigned KeyboardReports() const { return m_nKeyboardReports; }
	unsigned MouseReports() const { return m_nMouseReports; }

private:
	struct TKeyboardSlot {
		CUSBKeyboardDevice *volatile pDevice;
		unsigned nNumber; // N in "ukbdN"
		pimac::KeyboardTracker Tracker;
		PiInput *pOwner;
	};

	struct TMouseSlot {
		CMouseDevice *volatile pDevice;
		unsigned nNumber; // N in "mouseN"
		volatile unsigned nButtons;
		PiInput *pOwner;
	};

	void AttachKeyboard(unsigned nNumber);
	void AttachMouse(unsigned nNumber);
	unsigned CombinedButtons() const;

	static void KeyStatusHandlerRaw(unsigned char ucModifiers, const unsigned char RawKeys[6],
					void *pArg);
	static void MouseStatusHandler(unsigned nButtons, int nDisplacementX, int nDisplacementY,
				       int nWheelMove, void *pArg);
	static void KeyboardRemovedHandler(CDevice *pDevice, void *pContext);
	static void MouseRemovedHandler(CDevice *pDevice, void *pContext);

	CDeviceNameService *m_pDeviceNameService;
	TKeyboardSlot m_Keyboards[kMaxKeyboards];
	TMouseSlot m_Mice[kMaxMice];
	pimac::InputQueue m_Queue;
	volatile unsigned m_nKeyboardReports;
	volatile unsigned m_nMouseReports;
};
