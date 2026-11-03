#include "host/pi_net.h"

#include <circle/logger.h>
#include <circle/net/dnsclient.h>
#include <circle/net/in.h>
#include <circle/sched/scheduler.h>
#include <circle/string.h>
#include <circle/util.h>

namespace {
const char FromNet[] = "pinet";
const u16 kLtoUdpPort = 1954;
const u8 kLtoUdpGroup[] = {239, 192, 76, 84};
} // namespace

// --- LocalTalk over UDP -----------------------------------------------------

PiMulticastLink::PiMulticastLink() : m_pSocket(0), m_bFailed(false)
{
	m_Group.Set(kLtoUdpGroup);
}

PiMulticastLink::~PiMulticastLink()
{
	delete m_pSocket;
}

void PiMulticastLink::Poll(CNetSubSystem *pNet)
{
	if (m_pSocket || m_bFailed || !pNet || !pNet->IsRunning())
		return;
	CSocket *pSocket = new CSocket(pNet, IPPROTO_UDP);
	if (pSocket->Bind(kLtoUdpPort) < 0 || pSocket->SetOptionAddMembership(m_Group) < 0) {
		CLogger::Get()->Write(FromNet, LogWarning, "LocalTalk: cannot join 239.192.76.84");
		delete pSocket;
		m_bFailed = true;
		return;
	}
	m_pSocket = pSocket;
	CLogger::Get()->Write(FromNet, LogNotice, "LocalTalk on UDP 239.192.76.84:%u", kLtoUdpPort);
}

void PiMulticastLink::Send(const u8 *pData, unsigned nLength)
{
	if (m_pSocket)
		m_pSocket->SendTo(pData, nLength, MSG_DONTWAIT, m_Group, kLtoUdpPort);
}

unsigned PiMulticastLink::Receive(u8 *pData, unsigned nMax)
{
	if (!m_pSocket)
		return 0;
	CIPAddress Sender;
	u16 nPort;
	const int n = m_pSocket->ReceiveFrom(pData, nMax, MSG_DONTWAIT, &Sender, &nPort);
	return n > 0 ? (unsigned)n : 0;
}

// --- TCP for the modem --------------------------------------------------------

void PiStreamTask::Run()
{
	if (m_Job == JobConnect)
		m_pLink->ConnectTask();
	else
		m_pLink->ListenTask();
}

PiStreamLink::PiStreamLink()
	: m_pNet(0), m_nListenPort(0), m_pSocket(0), m_pPending(0), m_State(Idle), m_nPort(0),
	  m_bConnectRequested(false), m_nGeneration(0), m_nRxPos(0), m_nRxLength(0)
{
	m_Host[0] = '\0';
}

void PiStreamLink::Start(CNetSubSystem *pNet, u16 nListenPort)
{
	m_pNet = pNet;
	m_nListenPort = nListenPort;
	new PiStreamTask(this, PiStreamTask::JobConnect);
	if (nListenPort)
		new PiStreamTask(this, PiStreamTask::JobListen);
}

void PiStreamLink::ConnectTask()
{
	CScheduler *pScheduler = CScheduler::Get();
	for (;;) {
		while (!m_bConnectRequested)
			pScheduler->MsSleep(20);
		m_bConnectRequested = false;
		const unsigned nGeneration = m_nGeneration;
		char Host[sizeof m_Host];
		strcpy(Host, m_Host);
		const u16 nPort = m_nPort;

		CSocket *pSocket = 0;
		bool bOK = false;
		if (m_pNet->IsRunning()) {
			CIPAddress Address;
			CDNSClient DNS(m_pNet);
			if (DNS.Resolve(Host, &Address) && nGeneration == m_nGeneration) {
				pSocket = new CSocket(m_pNet, IPPROTO_TCP);
				bOK = pSocket->Connect(Address, nPort) >= 0;
			}
		}
		if (nGeneration != m_nGeneration) { // hung up meanwhile
			delete pSocket;
			continue;
		}
		if (bOK) {
			m_pSocket = pSocket;
			m_State = Connected;
			CLogger::Get()->Write(FromNet, LogNotice, "Modem connected to %s:%u", Host, nPort);
		} else {
			delete pSocket;
			m_State = Failed;
			CLogger::Get()->Write(FromNet, LogNotice, "Modem cannot reach %s:%u", Host, nPort);
		}
	}
}

void PiStreamLink::ListenTask()
{
	CScheduler *pScheduler = CScheduler::Get();
	while (!m_pNet->IsRunning())
		pScheduler->MsSleep(500);
	CSocket Listener(m_pNet, IPPROTO_TCP);
	if (Listener.Bind(m_nListenPort) < 0 || Listener.Listen(1) < 0) {
		CLogger::Get()->Write(FromNet, LogWarning, "Modem cannot listen on port %u",
				      m_nListenPort);
		return;
	}
	CLogger::Get()->Write(FromNet, LogNotice, "Modem answers TCP port %u", m_nListenPort);
	for (;;) {
		CIPAddress Caller;
		u16 nPort;
		CSocket *pCaller = Listener.Accept(&Caller, &nPort);
		if (!pCaller)
			continue;
		if (m_pPending || m_State == Connected || m_State == Connecting) {
			delete pCaller; // busy
			continue;
		}
		m_pPending = pCaller;
		// Wait until the Mac answers or turns the call away.
		while (m_pPending == pCaller)
			pScheduler->MsSleep(50);
	}
}

void PiStreamLink::Connect(const char *pHost, u16 nPort)
{
	Close();
	strncpy(m_Host, pHost, sizeof m_Host - 1);
	m_Host[sizeof m_Host - 1] = '\0';
	m_nPort = nPort;
	m_State = Connecting;
	m_bConnectRequested = true;
}

bool PiStreamLink::Incoming()
{
	return m_pPending && m_State != Connected && m_State != Connecting;
}

void PiStreamLink::Accept()
{
	if (!m_pPending)
		return;
	Close();
	m_pSocket = m_pPending;
	m_pPending = 0;
	m_State = Connected;
	CLogger::Get()->Write(FromNet, LogNotice, "Modem answered a call");
}

void PiStreamLink::Close()
{
	++m_nGeneration;
	m_bConnectRequested = false;
	delete m_pSocket;
	m_pSocket = 0;
	m_nRxPos = m_nRxLength = 0;
	if (m_State != Connected && m_State != Connecting && m_pPending) {
		delete m_pPending; // turn the caller away
		m_pPending = 0;
	}
	m_State = Idle;
}

void PiStreamLink::Fail()
{
	delete m_pSocket;
	m_pSocket = 0;
	m_State = Failed;
}

unsigned PiStreamLink::Send(const u8 *pData, unsigned nLength)
{
	if (m_State != Connected || !m_pSocket)
		return 0;
	const int n = m_pSocket->Send(pData, nLength, MSG_DONTWAIT);
	if (n < 0) {
		Fail();
		return 0;
	}
	return (unsigned)n;
}

unsigned PiStreamLink::Receive(u8 *pData, unsigned nMax)
{
	if (m_State != Connected || !m_pSocket)
		return 0;
	if (m_nRxPos == m_nRxLength) {
		// One whole segment per receive (a shorter buffer would lose the rest).
		const int n = m_pSocket->Receive(m_RxBuffer, sizeof m_RxBuffer, MSG_DONTWAIT);
		if (n < 0) {
			Fail();
			return 0;
		}
		m_nRxPos = 0;
		m_nRxLength = (unsigned)n;
	}
	unsigned n = m_nRxLength - m_nRxPos;
	if (n > nMax)
		n = nMax;
	memcpy(pData, m_RxBuffer + m_nRxPos, n);
	m_nRxPos += n;
	return n;
}
