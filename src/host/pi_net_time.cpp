#include "host/pi_net_time.h"

#include <circle/logger.h>
#include <circle/net/dnsclient.h>
#include <circle/net/ipaddress.h>
#include <circle/net/ntpclient.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>

#include "core/compat.h"

namespace {
const char FromNetTime[] = "nettime";
const unsigned kRefreshSeconds = 3600;  // after an answer
const unsigned kFirstRetrySeconds = 15; // after a failure, doubling...
const unsigned kMaxRetrySeconds = 600;  // ...up to this
const unsigned kEarliest = 1704067200;  // 2024-01-01: anything before is a bad answer
} // namespace

PiNetTime::PiNetTime(CNetSubSystem *pNet)
	: m_pNet(pNet), m_bServerChanged(false), m_nFixes(0), m_nFixUptime(0), m_bLastFailed(false)
{
	m_Server[0] = '\0';
}

void PiNetTime::SetServer(const char *pServer)
{
	if (pimac::StrEqual(m_Server, pServer))
		return;
	pimac::StrCopy(m_Server, pServer, sizeof m_Server);
	m_bServerChanged = true;
}

unsigned PiNetTime::UtcNow() const
{
	return m_nFixes ? CTimer::Get()->GetUniversalTime() : 0;
}

unsigned PiNetTime::SecondsSinceFix() const
{
	return CTimer::Get()->GetUptime() - m_nFixUptime;
}

bool PiNetTime::Query()
{
	char Server[sizeof m_Server];
	pimac::StrCopy(Server, m_Server, sizeof Server);
	CIPAddress Address;
	CDNSClient DNS(m_pNet);
	if (!DNS.Resolve(Server, &Address)) {
		CLogger::Get()->Write(FromNetTime, LogWarning, "Cannot resolve %s", Server);
		return false;
	}
	CNTPClient NTP(m_pNet);
	const unsigned nTime = NTP.GetTime(Address); // up to ~3 s; 0 on failure
	if (nTime < kEarliest) {
		CLogger::Get()->Write(FromNetTime, LogWarning, "No time from %s", Server);
		return false;
	}
	if (m_bServerChanged)
		return false; // asked the old server: ask the new one instead
	CTimer::Get()->SetTime(nTime, FALSE);
	m_nFixUptime = CTimer::Get()->GetUptime();
	++m_nFixes;
	CLogger::Get()->Write(FromNetTime, LogNotice, "Time from %s: %u (UTC)", Server, nTime);
	return true;
}

void PiNetTime::Run()
{
	CScheduler *pScheduler = CScheduler::Get();
	unsigned nRetry = kFirstRetrySeconds;
	for (;;) {
		while (!m_Server[0] || !m_pNet->IsRunning())
			pScheduler->MsSleep(500);
		m_bServerChanged = false;
		const bool bOK = Query();
		m_bLastFailed = !bOK;
		unsigned nWait = kRefreshSeconds;
		if (bOK) {
			nRetry = kFirstRetrySeconds;
		} else if (!m_bServerChanged) {
			nWait = nRetry;
			nRetry = nRetry * 2 > kMaxRetrySeconds ? kMaxRetrySeconds : nRetry * 2;
		}
		for (unsigned s = 0; s < nWait && !m_bServerChanged; ++s)
			pScheduler->Sleep(1);
	}
}
