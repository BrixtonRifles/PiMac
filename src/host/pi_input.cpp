#include "host/pi_input.h"

#include <circle/logger.h>
#include <circle/string.h>

static const char FromInput[] = "input";

PiInput::PiInput(CDeviceNameService *pDeviceNameService)
	: m_pDeviceNameService(pDeviceNameService), m_nKeyboardReports(0), m_nMouseReports(0)
{
	for (unsigned i = 0; i < kMaxKeyboards; ++i) {
		m_Keyboards[i].pDevice = 0;
		m_Keyboards[i].nNumber = 0;
		m_Keyboards[i].pOwner = this;
	}
	for (unsigned i = 0; i < kMaxMice; ++i) {
		m_Mice[i].pDevice = 0;
		m_Mice[i].nNumber = 0;
		m_Mice[i].nButtons = 0;
		m_Mice[i].pOwner = this;
	}
}

void PiInput::UpdateDevices()
{
	for (unsigned n = 1; n <= kMaxDeviceNumber; ++n) {
		AttachKeyboard(n);
		AttachMouse(n);
	}
}

void PiInput::AttachKeyboard(unsigned nNumber)
{
	CString Name;
	Name.Format("ukbd%u", nNumber);
	CUSBKeyboardDevice *pDevice =
		(CUSBKeyboardDevice *)m_pDeviceNameService->GetDevice(Name, FALSE);
	if (!pDevice)
		return;

	TKeyboardSlot *pFree = 0;
	for (unsigned i = 0; i < kMaxKeyboards; ++i) {
		if (m_Keyboards[i].pDevice == pDevice)
			return; // already attached
		if (!m_Keyboards[i].pDevice && !pFree)
			pFree = &m_Keyboards[i];
	}
	if (!pFree) {
		CLogger::Get()->Write(FromInput, LogWarning, "Too many keyboards, ignoring %s",
				      (const char *)Name);
		return;
	}

	pFree->nNumber = nNumber;
	pFree->pDevice = pDevice;
	pDevice->RegisterRemovedHandler(KeyboardRemovedHandler, pFree);
	pDevice->RegisterKeyStatusHandlerRaw(KeyStatusHandlerRaw, FALSE, pFree);
	CLogger::Get()->Write(FromInput, LogNotice, "Using keyboard %s", (const char *)Name);
}

void PiInput::AttachMouse(unsigned nNumber)
{
	CString Name;
	Name.Format("mouse%u", nNumber);
	CMouseDevice *pDevice = (CMouseDevice *)m_pDeviceNameService->GetDevice(Name, FALSE);
	if (!pDevice)
		return;

	TMouseSlot *pFree = 0;
	for (unsigned i = 0; i < kMaxMice; ++i) {
		if (m_Mice[i].pDevice == pDevice)
			return;
		if (!m_Mice[i].pDevice && !pFree)
			pFree = &m_Mice[i];
	}
	if (!pFree) {
		CLogger::Get()->Write(FromInput, LogWarning, "Too many mice, ignoring %s",
				      (const char *)Name);
		return;
	}

	pFree->nNumber = nNumber;
	pFree->nButtons = 0;
	pFree->pDevice = pDevice;
	pDevice->RegisterRemovedHandler(MouseRemovedHandler, pFree);
	pDevice->RegisterStatusHandler(MouseStatusHandler, pFree);
	CLogger::Get()->Write(FromInput, LogNotice, "Using mouse %s", (const char *)Name);
}

unsigned PiInput::KeyboardCount() const
{
	unsigned n = 0;
	for (unsigned i = 0; i < kMaxKeyboards; ++i)
		if (m_Keyboards[i].pDevice)
			++n;
	return n;
}

unsigned PiInput::MouseCount() const
{
	unsigned n = 0;
	for (unsigned i = 0; i < kMaxMice; ++i)
		if (m_Mice[i].pDevice)
			++n;
	return n;
}

unsigned PiInput::CombinedButtons() const
{
	unsigned nButtons = 0;
	for (unsigned i = 0; i < kMaxMice; ++i)
		if (m_Mice[i].pDevice)
			nButtons |= m_Mice[i].nButtons;
	return nButtons;
}

void PiInput::KeyStatusHandlerRaw(unsigned char ucModifiers, const unsigned char RawKeys[6],
				  void *pArg)
{
	TKeyboardSlot *pSlot = (TKeyboardSlot *)pArg;
	PiInput *pThis = pSlot->pOwner;
	pThis->m_nKeyboardReports = pThis->m_nKeyboardReports + 1;
	pSlot->Tracker.Update(ucModifiers, RawKeys, pThis->m_Queue);
}

void PiInput::MouseStatusHandler(unsigned nButtons, int nDisplacementX, int nDisplacementY,
				 int nWheelMove, void *pArg)
{
	(void)nWheelMove;
	TMouseSlot *pSlot = (TMouseSlot *)pArg;
	PiInput *pThis = pSlot->pOwner;
	pThis->m_nMouseReports = pThis->m_nMouseReports + 1;
	pSlot->nButtons = nButtons;
	pimac::PushMouse(pThis->m_Queue, pThis->CombinedButtons(), nDisplacementX, nDisplacementY);
}

void PiInput::KeyboardRemovedHandler(CDevice *pDevice, void *pContext)
{
	(void)pDevice;
	TKeyboardSlot *pSlot = (TKeyboardSlot *)pContext;
	pSlot->Tracker.ReleaseAll(pSlot->pOwner->m_Queue);
	pSlot->pDevice = 0;
	CLogger::Get()->Write(FromInput, LogNotice, "Keyboard ukbd%u removed", pSlot->nNumber);
}

void PiInput::MouseRemovedHandler(CDevice *pDevice, void *pContext)
{
	(void)pDevice;
	TMouseSlot *pSlot = (TMouseSlot *)pContext;
	pSlot->pDevice = 0;
	pSlot->nButtons = 0;
	PiInput *pThis = pSlot->pOwner;
	pimac::PushMouse(pThis->m_Queue, pThis->CombinedButtons(), 0, 0);
	CLogger::Get()->Write(FromInput, LogNotice, "Mouse mouse%u removed", pSlot->nNumber);
}
