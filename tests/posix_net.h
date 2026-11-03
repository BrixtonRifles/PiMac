// POSIX implementations of core/net.h for hostmac: LocalTalk-over-UDP
// multicast and non-blocking TCP for the network modem.
#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/net.h"

namespace pimac {

class PosixMulticast : public DatagramLink {
public:
	bool Open(const char *group, u16 port)
	{
		m_Fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (m_Fd < 0)
			return false;
		int one = 1;
		setsockopt(m_Fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		setsockopt(m_Fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons(port);
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(m_Fd, (sockaddr *)&a, sizeof a) < 0) {
			perror("localtalk bind");
			return false;
		}
		ip_mreq m = {};
		m.imr_multiaddr.s_addr = inet_addr(group);
		m.imr_interface.s_addr = htonl(INADDR_ANY);
		if (setsockopt(m_Fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) < 0) {
			perror("localtalk join");
			return false;
		}
		u_char loop = 1, ttl = 1;
		setsockopt(m_Fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
		setsockopt(m_Fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
		fcntl(m_Fd, F_SETFL, O_NONBLOCK);
		m_Group = a;
		m_Group.sin_addr.s_addr = inet_addr(group);
		return true;
	}
	void Send(const u8 *data, unsigned len) override
	{
		if (m_Debug)
			Dump("send", data, len);
		sendto(m_Fd, data, len, 0, (sockaddr *)&m_Group, sizeof m_Group);
	}
	unsigned Receive(u8 *data, unsigned max) override
	{
		const ssize_t n = recv(m_Fd, data, max, 0);
		if (n > 0 && m_Debug)
			Dump("recv", data, (unsigned)n);
		return n > 0 ? (unsigned)n : 0;
	}
	void SetDebug(bool on) { m_Debug = on; }

private:
	static void Dump(const char *what, const u8 *data, unsigned len)
	{
		fprintf(stderr, "LT %s %3u:", what, len);
		for (unsigned i = 0; i < len && i < 32; ++i)
			fprintf(stderr, " %02X", data[i]);
		fprintf(stderr, "%s\n", len > 32 ? " ..." : "");
	}

	int m_Fd = -1;
	bool m_Debug = false;
	sockaddr_in m_Group = {};
};

// One TCP connection plus an optional listening socket.
class PosixStream : public StreamLink {
public:
	bool Listen(u16 port)
	{
		m_Listen = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(m_Listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_port = htons(port);
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(m_Listen, (sockaddr *)&a, sizeof a) < 0 || listen(m_Listen, 1) < 0) {
			perror("modem listen");
			close(m_Listen);
			m_Listen = -1;
			return false;
		}
		fcntl(m_Listen, F_SETFL, O_NONBLOCK);
		return true;
	}

	void Connect(const char *host, u16 port) override
	{
		Close();
		addrinfo hints = {}, *res = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		char service[8];
		snprintf(service, sizeof service, "%u", port);
		if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
			m_State = Failed;
			return;
		}
		m_Fd = socket(AF_INET, SOCK_STREAM, 0);
		fcntl(m_Fd, F_SETFL, O_NONBLOCK);
		const int r = connect(m_Fd, res->ai_addr, res->ai_addrlen);
		freeaddrinfo(res);
		if (r == 0)
			m_State = Connected;
		else if (errno == EINPROGRESS)
			m_State = Connecting;
		else
			Fail();
	}
	bool Incoming() override
	{
		if (m_Pending < 0 && m_Listen >= 0) {
			m_Pending = accept(m_Listen, nullptr, nullptr);
			if (m_Pending >= 0)
				fcntl(m_Pending, F_SETFL, O_NONBLOCK);
		}
		return m_Pending >= 0 && m_State != Connected && m_State != Connecting;
	}
	void Accept() override
	{
		if (m_Pending < 0)
			return;
		Close();
		m_Fd = m_Pending;
		m_Pending = -1;
		m_State = Connected;
	}
	void Close() override
	{
		if (m_Fd >= 0)
			close(m_Fd);
		m_Fd = -1;
		if (m_State != Connected && m_State != Connecting && m_Pending >= 0) {
			close(m_Pending); // turn away the caller
			m_Pending = -1;
		}
		m_State = Idle;
	}
	State GetState() override
	{
		if (m_State == Connecting) {
			pollConnect();
		}
		return m_State;
	}
	unsigned Send(const u8 *data, unsigned len) override
	{
		if (GetState() != Connected)
			return 0;
		const ssize_t n = send(m_Fd, data, len, 0);
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			Fail();
		return n > 0 ? (unsigned)n : 0;
	}
	unsigned Receive(u8 *data, unsigned max) override
	{
		if (GetState() != Connected)
			return 0;
		const ssize_t n = recv(m_Fd, data, max, 0);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
			Fail();
		return n > 0 ? (unsigned)n : 0;
	}

private:
	void Fail()
	{
		if (m_Fd >= 0)
			close(m_Fd);
		m_Fd = -1;
		m_State = Failed;
	}
	void pollConnect()
	{
		fd_set w;
		FD_ZERO(&w);
		FD_SET(m_Fd, &w);
		timeval tv = {0, 0};
		if (select(m_Fd + 1, nullptr, &w, nullptr, &tv) <= 0)
			return;
		int err = 0;
		socklen_t len = sizeof err;
		getsockopt(m_Fd, SOL_SOCKET, SO_ERROR, &err, &len);
		if (err)
			Fail();
		else
			m_State = Connected;
	}

	int m_Fd = -1, m_Listen = -1, m_Pending = -1;
	State m_State = Idle;
};

} // namespace pimac
