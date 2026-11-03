// LocalTalk on the printer port, carried over UDP multicast ("LToUDP", as in
// Mini vMac and tashrouter): each datagram is a 4-byte sender ID (big-endian)
// followed by one LLAP frame (destination, source, type, data) without its
// CRC. Group 239.192.76.84, port 1954.
//
// LLAP's RTS/CTS handshake has to complete within microseconds, which a
// network cannot do, so it stays local: an RTS the Mac sends to another node
// is answered at once with a CTS from that node, and neither goes out.
// Collisions are the network's problem. ENQ/ACK (address acquisition) and all
// data frames go out as they are.
#pragma once

#include "core/net.h"
#include "mac/scc.h"

namespace pimac {

class LocalTalkPort : public SccPort {
public:
	enum : u8 { kLapEnq = 0x81, kLapAck = 0x82, kLapRts = 0x84, kLapCts = 0x85 };

	LocalTalkPort();

	// Not owned. The sender ID tells our own datagrams apart when the group
	// loops them back; it should differ between emulators on one network.
	void SetLink(DatagramLink *link, u32 senderId);

	unsigned FramesSent() const { return m_Sent; }
	unsigned FramesReceived() const { return m_Received; }
	u8 NodeAddress() const { return m_Node; }

	// SccPort
	void SendFrame(const u8 *frame, unsigned len) override;
	unsigned ReceiveFrame(u8 *frame, unsigned max) override;
	void SetNodeAddress(u8 node) override { m_Node = node; }

private:
	enum : unsigned { kHeader = 4, kMaxDatagram = kHeader + Scc::kMaxFrame };

	DatagramLink *m_Link;
	u32 m_SenderId;
	bool m_CtsPending;
	u8 m_Cts[3];
	u8 m_Node;
	unsigned m_Sent, m_Received;
};

} // namespace pimac
