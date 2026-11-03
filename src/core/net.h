// Network connections as the emulated serial devices see them. The host (the
// Pi's Circle network stack, or POSIX sockets in hostmac) implements these;
// every call must return at once.
#pragma once

#include "core/compat.h"

namespace pimac {

// UDP datagrams to and from a multicast group (LocalTalk over UDP).
class DatagramLink {
public:
	virtual ~DatagramLink() {}
	virtual void Send(const u8 *data, unsigned len) = 0;
	// Copies the next datagram received; 0 if none.
	virtual unsigned Receive(u8 *data, unsigned max) = 0;
};

// One TCP connection, made outwards or accepted from a listening port.
class StreamLink {
public:
	enum State { Idle, Connecting, Connected, Failed };

	virtual ~StreamLink() {}
	// Starts connecting (host name or dotted address). State() goes
	// Connecting, then Connected or Failed.
	virtual void Connect(const char *host, u16 port) = 0;
	// Someone connected to the listening port and waits to be accepted.
	virtual bool Incoming() = 0;
	virtual void Accept() = 0; // State() becomes Connected
	virtual void Close() = 0;  // hang up, or turn away an incoming call
	virtual State GetState() = 0; // Failed also when the far end hung up
	// Returns how many bytes were taken/copied (0 if none, or not connected).
	virtual unsigned Send(const u8 *data, unsigned len) = 0;
	virtual unsigned Receive(u8 *data, unsigned max) = 0;
};

} // namespace pimac
