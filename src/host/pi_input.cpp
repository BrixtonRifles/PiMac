#include "host/pi_input.h"

#include <circle/logger.h>

static const char FromInput[] = "input";

PiInput *PiInput::s_pThis = 0;

PiInput::PiInput(CDeviceNameService *pDeviceNameService)
	: m_pDeviceNameService(pDeviceNameService), m_pKeyboard(0), m_pMouse(0)
{
	s_pThis = this;
}

void PiInput::UpdateDevices()
{
	if (!m_pKeyboard) {
		m_pKeyboard = (CUSBKeyboardDevice *)m_pDeviceNameService->GetDevice("ukbd1", FALSE);
		if (m_pKeyboard) {
			m_pKeyboard->RegisterRemovedHandler(KeyboardRemovedHandler);
			m_pKeyboard->RegisterKeyStatusHandlerRaw(KeyStatusHandlerRaw);
			CLogger::Get()->Write(FromInput, LogNotice, "USB keyboard attached");
		}
	}

	if (!m_pMouse) {
		m_pMouse = (CMouseDevice *)m_pDeviceNameService->GetDevice("mouse1", FALSE);
		if (m_pMouse) {
			m_pMouse->RegisterRemovedHandler(MouseRemovedHandler);
			m_pMouse->RegisterStatusHandler(MouseStatusHandler);
			CLogger::Get()->Write(FromInput, LogNotice, "USB mouse attached");
		}
	}
}

void PiInput::KeyStatusHandlerRaw(unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	s_pThis->m_Tracker.Update(ucModifiers, RawKeys, s_pThis->m_Queue);
}

void PiInput::MouseStatusHandler(unsigned nButtons, int nDisplacementX, int nDisplacementY,
				 int nWheelMove)
{
	(void)nWheelMove;
	pimac::PushMouse(s_pThis->m_Queue, nButtons, nDisplacementX, nDisplacementY);
}

void PiInput::KeyboardRemovedHandler(CDevice *pDevice, void *pContext)
{
	(void)pDevice;
	(void)pContext;
	s_pThis->m_Tracker.ReleaseAll(s_pThis->m_Queue);
	s_pThis->m_pKeyboard = 0;
	CLogger::Get()->Write(FromInput, LogNotice, "USB keyboard removed");
}

void PiInput::MouseRemovedHandler(CDevice *pDevice, void *pContext)
{
	(void)pDevice;
	(void)pContext;
	pimac::PushMouse(s_pThis->m_Queue, 0, 0, 0);
	s_pThis->m_pMouse = 0;
	CLogger::Get()->Write(FromInput, LogNotice, "USB mouse removed");
}
