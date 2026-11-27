// Internet time for the Mac's clock (the Pi has no battery-backed clock).
// A background task waits for the network, asks an NTP server, sets the
// Pi's own clock (Circle's CTimer, UTC) and asks again every hour; the
// kernel turns that into each Mac's local time (mac/clock_sync.h). Nothing
// here ever makes the emulation wait.
#pragma once

#include <circle/net/netsubsystem.h>
#include <circle/sched/task.h>

class PiNetTime : public CTask {
public:
	explicit PiNetTime(CNetSubSystem *pNet);

	// The server to ask ("" = none: the task stays idle). A new name is
	// asked at once.
	void SetServer(const char *pServer);
	const char *Server() const { return m_Server; }

	// UTC now (seconds since 1970) once a server has answered, else 0.
	unsigned UtcNow() const;
	unsigned Fixes() const { return m_nFixes; } // answers so far
	unsigned SecondsSinceFix() const;           // since the last answer
	bool LastFailed() const { return m_bLastFailed; } // the last attempt got no time

	void Run() override;

private:
	bool Query(); // one attempt: DNS + NTP; sets the Pi's clock

	CNetSubSystem *m_pNet;
	char m_Server[64];
	volatile bool m_bServerChanged;
	volatile unsigned m_nFixes;
	volatile unsigned m_nFixUptime; // CTimer uptime of the last answer
	volatile bool m_bLastFailed;
};
