// The Pi's network (Circle: LAN7800/SMSC Ethernet, DHCP) for the emulated
// serial ports: LocalTalk over UDP multicast on the printer port, and TCP for
// the network modem on the modem port.
//
// Circle's network runs in cooperative tasks: the main loop must call
// CScheduler::Yield() (it does, while waiting for the next frame). Blocking
// work (DNS, connect, accept) runs in tasks of its own here, so nothing the
// emulation calls ever waits.
#pragma once

#include <circle/net/ipaddress.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/netdevice.h>
#include <circle/sched/task.h>

#include "core/net.h"

// LToUDP: 239.192.76.84, port 1954.
class PiMulticastLink : public pimac::DatagramLink {
public:
	PiMulticastLink();
	~PiMulticastLink();

	void Poll(CNetSubSystem *pNet); // opens the socket once the network is up
	bool IsOpen() const { return m_pSocket != 0; }

	void Send(const u8 *pData, unsigned nLength) override;
	unsigned Receive(u8 *pData, unsigned nMax) override;

private:
	CSocket *m_pSocket;
	CIPAddress m_Group;
	bool m_bFailed;
};

class PiStreamLink;

// Runs a PiStreamLink's blocking calls.
class PiStreamTask : public CTask {
public:
	enum Job { JobConnect, JobListen };
	PiStreamTask(PiStreamLink *pLink, Job job) : m_pLink(pLink), m_Job(job) {}
	void Run() override;

private:
	PiStreamLink *m_pLink;
	Job m_Job;
};

class PiStreamLink : public pimac::StreamLink {
public:
	PiStreamLink();

	// nListenPort 0: outgoing calls only. Starts the tasks.
	void Start(CNetSubSystem *pNet, u16 nListenPort);

	void Connect(const char *pHost, u16 nPort) override;
	bool Incoming() override;
	void Accept() override;
	void Close() override;
	State GetState() override { return m_State; }
	unsigned Send(const u8 *pData, unsigned nLength) override;
	unsigned Receive(u8 *pData, unsigned nMax) override;

private:
	friend class PiStreamTask;
	void ConnectTask();
	void ListenTask();
	void Fail();

	CNetSubSystem *m_pNet;
	u16 m_nListenPort;
	CSocket *m_pSocket;   // the connection
	CSocket *m_pPending;  // an accepted caller not yet answered
	volatile State m_State;
	// Connect request for the task; m_nGeneration tells it if the call was
	// hung up (or redialled) while it was connecting.
	char m_Host[64];
	u16 m_nPort;
	volatile bool m_bConnectRequested;
	volatile unsigned m_nGeneration;
	// Circle hands over whole TCP segments: what the modem has not taken yet.
	u8 m_RxBuffer[FRAME_BUFFER_SIZE];
	unsigned m_nRxPos, m_nRxLength;
};
