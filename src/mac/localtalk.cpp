#include "mac/localtalk.h"

namespace pimac {

LocalTalkPort::LocalTalkPort()
	: m_Link(0), m_SenderId(0), m_CtsPending(false), m_Node(0), m_Sent(0), m_Received(0)
{
	memset(m_Cts, 0, sizeof m_Cts);
}

void LocalTalkPort::SetLink(DatagramLink *link, u32 senderId)
{
	m_Link = link;
	m_SenderId = senderId;
}

void LocalTalkPort::SendFrame(const u8 *frame, unsigned len)
{
	if (len < 3)
		return;
	const u8 dst = frame[0], src = frame[1], type = frame[2];
	if (type == kLapRts) {
		// Answer for the other node: CTS from dst to us.
		if (dst != 0xFF) {
			m_Cts[0] = src;
			m_Cts[1] = dst;
			m_Cts[2] = kLapCts;
			m_CtsPending = true;
		}
		return;
	}
	if (type == kLapCts)
		return; // the sender's own emulator answered its RTS already
	if (!m_Link)
		return;
	u8 datagram[kMaxDatagram];
	datagram[0] = (u8)(m_SenderId >> 24);
	datagram[1] = (u8)(m_SenderId >> 16);
	datagram[2] = (u8)(m_SenderId >> 8);
	datagram[3] = (u8)m_SenderId;
	if (len > Scc::kMaxFrame)
		len = Scc::kMaxFrame;
	memcpy(datagram + kHeader, frame, len);
	m_Link->Send(datagram, kHeader + len);
	++m_Sent;
}

unsigned LocalTalkPort::ReceiveFrame(u8 *frame, unsigned max)
{
	if (m_CtsPending) {
		m_CtsPending = false;
		if (max < 3)
			return 0;
		memcpy(frame, m_Cts, 3);
		return 3;
	}
	if (!m_Link)
		return 0;
	u8 datagram[kMaxDatagram];
	for (;;) {
		const unsigned n = m_Link->Receive(datagram, sizeof datagram);
		if (n == 0)
			return 0;
		if (n < kHeader + 3)
			continue; // too short for an LLAP frame
		const u32 id = ((u32)datagram[0] << 24) | ((u32)datagram[1] << 16) |
			       ((u32)datagram[2] << 8) | datagram[3];
		if (id == m_SenderId)
			continue; // our own, looped back by the group
		unsigned len = n - kHeader;
		if (len > max)
			len = max;
		memcpy(frame, datagram + kHeader, len);
		++m_Received;
		return len;
	}
}

} // namespace pimac
