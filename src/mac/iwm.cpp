#include "mac/iwm.h"

#include "core/gcr.h"
#include "core/log.h"

namespace pimac {

// Drive registers by CA2 CA1 CA0 SEL (bit 3..0).
enum DriveRegister : unsigned {
	RegDirection = 0x0, // DIRTN   0 = step inward
	RegDiskIn = 0x1,    // CSTIN   0 = disk inserted        (SEL)
	RegStep = 0x2,      // STEP    0 = still stepping       (CA0)
	RegLocked = 0x3,    // WRTPRT  0 = write protected      (CA0 SEL)
	RegMotor = 0x4,     // MOTORON 0 = motor on             (CA1)
	RegTrack0 = 0x5,    // TK0     0 = head at track 0      (CA1 SEL)
	RegSwitched = 0x6,  //         1 = new disk inserted    (CA1 CA0)
	RegTach = 0x7,      // TACH    60 pulses per revolution (CA1 CA0 SEL)
	RegHead0 = 0x8,     // RDDATA0 select lower head        (CA2)
	RegHead1 = 0x9,     // RDDATA1 select upper head        (CA2 SEL)
	RegSides = 0xC,     // SIDES   0 = single-sided drive   (CA2 CA1)
	RegReady = 0xD,     //         0 = ready                (CA2 CA1 SEL)
	RegInstalled = 0xE, // DRVIN   0 = drive installed      (CA2 CA1 CA0)
	RegHighDensity = 0xF, //       0 = HD (SuperDrive) disk (all)
};

// Drive commands, strobed with LSTRB; CA2 is the argument.
enum DriveCommand : unsigned {
	CmdDirection = 0x0,    // CA2: 0 inward, 1 outward
	CmdStep = 0x2,         // CA0
	CmdMotor = 0x4,        // CA1, CA2: 0 on, 1 off
	CmdEject = 0x6,        // CA1 CA0 (with CA2 set)
	CmdResetSwitched = 0x1 // SEL (with CA2 set)
};

Iwm::Iwm()
	: m_Image(0), m_Ejected(false), m_SectorsRead(0), m_SectorsWritten(0), m_BadWrites(0)
{
	Reset();
}

void Iwm::Reset()
{
	m_Lines = 0;
	m_Sel = false;
	m_Mode = 0;
	m_Track = 0;
	m_StepDir = 1;
	m_Head = 0;
	m_MotorOn = false;
	m_Switched = m_Image != 0;
	m_TrackLen = m_TrackPos = 0;
	m_CachedTrack = -1;
	m_CachedHead = 0;
	m_TrackWritten = false;
	m_Mark = 0;
}

void Iwm::Insert(FloppyImage *image)
{
	m_Image = image;
	m_Switched = true;
	m_Ejected = false;
	m_CachedTrack = -1;
}

void Iwm::Remove()
{
	if (m_TrackWritten)
		FinishWrite();
	m_Image = 0;
	m_CachedTrack = -1;
}

unsigned Iwm::Register() const
{
	return ((m_Lines & kCA2) ? 8u : 0u) | ((m_Lines & kCA1) ? 4u : 0u) |
	       ((m_Lines & kCA0) ? 2u : 0u) | (m_Sel ? 1u : 0u);
}

void Iwm::SetSel(bool sel)
{
	m_Sel = sel;
	const unsigned reg = Register();
	if (reg == RegHead0 || reg == RegHead1)
		m_Head = reg & 1;
}

bool Iwm::Sense(u64 now)
{
	if (m_Lines & kExternal)
		return true; // no external drive: everything reads 1 ("not installed")

	switch (Register()) {
	case RegDirection:
		return m_StepDir < 0;
	case RegDiskIn:
		return m_Image == 0;
	case RegStep:
		return true; // steps complete instantly
	case RegLocked:
		return m_Image != 0 && m_Image->Writable();
	case RegMotor:
		return !m_MotorOn;
	case RegTrack0:
		return m_Track != 0;
	case RegSwitched:
		return m_Switched;
	case RegTach: {
		// The 800K drive spins faster on the inner zones (394..590 rpm),
		// producing 60 pulses per revolution.
		static const unsigned kRpm[5] = {394, 429, 472, 525, 590};
		const unsigned rpm = kRpm[m_Track >> 4];
		const u64 halfPeriod = 7833600ull / (rpm * 2); // 60 pulses/rev, rpm/60 rev/s
		return ((now / halfPeriod) & 1) != 0;
	}
	case RegHead0:
	case RegHead1:
		return false;
	case RegSides:
		return true; // double-sided (800K) drive
	case RegReady:
		return false;
	case RegInstalled:
		return false;
	case RegHighDensity:
		return true; // not a SuperDrive / no HD disk
	default:
		return true;
	}
}

void Iwm::Command()
{
	if (m_Lines & kExternal)
		return;
	const bool ca2 = (m_Lines & kCA2) != 0;
	switch (Register() & 0x7) { // CA1 CA0 SEL
	case CmdDirection:
		m_StepDir = ca2 ? -1 : 1;
		break;
	case CmdStep:
		m_Track += m_StepDir;
		if (m_Track < 0)
			m_Track = 0;
		if (m_Track >= (int)gcr::kTracks)
			m_Track = gcr::kTracks - 1;
		break;
	case CmdMotor:
		m_MotorOn = !ca2;
		break;
	case CmdEject:
		if (ca2 && m_Image) {
			if (m_TrackWritten)
				FinishWrite();
			m_Image = 0;
			m_Ejected = true;
			m_MotorOn = false;
			m_CachedTrack = -1;
		}
		break;
	case CmdResetSwitched:
		if (ca2)
			m_Switched = false;
		break;
	default:
		break;
	}
}

void Iwm::LinesChanged(unsigned old)
{
	const unsigned changed = old ^ m_Lines;
	if ((changed & kLSTRB) && (m_Lines & kLSTRB))
		Command();
	const unsigned reg = Register();
	if (reg == RegHead0 || reg == RegHead1)
		m_Head = reg & 1;
	// Leaving write mode: decode what the Mac wrote.
	if ((changed & kQ7) && !(m_Lines & kQ7) && m_TrackWritten)
		FinishWrite();
}

// Nibbles that pass the head in one revolution, per zone: the 800K drive
// spins at 394..590 rpm and data moves at 489.6 kbit/s.
static const unsigned kRingNibbles[5] = {9320, 8559, 7779, 6994, 6223};

void Iwm::BuildTrack()
{
	m_CachedTrack = m_Track;
	m_CachedHead = m_Head;
	m_TrackLen = kRingNibbles[m_Track >> 4];
	if (m_TrackPos >= m_TrackLen)
		m_TrackPos = 0;
	memset(m_TrackData, 0xFF, m_TrackLen);
	if (!m_Image || m_Head >= m_Image->Sides())
		return; // unformatted: all sync

	const unsigned spt = gcr::SectorsPerTrack((unsigned)m_Track);
	const unsigned first =
		gcr::TrackStartBlock((unsigned)m_Track, m_Image->Sides()) + m_Head * spt;

	// 2:1 interleave, as the Mac formats disks.
	int order[12];
	bool used[12] = {false};
	unsigned slot = 0;
	for (unsigned s = 0; s < spt; ++s) {
		while (used[slot])
			slot = (slot + 1) % spt;
		order[slot] = (int)s;
		used[slot] = true;
		slot = (slot + 2) % spt;
	}

	// Spread the spare room evenly as sync gaps before each sector.
	const unsigned sectorNibbles = gcr::kAddressNibbles + 6 + gcr::kDataNibbles;
	const unsigned gap = (m_TrackLen - spt * sectorNibbles) / spt;
	u8 sector[524];
	u8 *p = m_TrackData;
	for (unsigned i = 0; i < spt; ++i) {
		const unsigned s = (unsigned)order[i];
		p += gap;
		gcr::EncodeAddress(p, (unsigned)m_Track, s, m_Head, m_Image->FormatByte());
		p += gcr::kAddressNibbles + 6;
		m_Image->ReadSector(first + s, sector);
		gcr::EncodeData(p, s, sector);
		p += gcr::kDataNibbles;
	}
}

bool Iwm::TrackReady()
{
	if (!m_Image || !m_MotorOn || (m_Lines & kExternal))
		return false;
	if (m_CachedTrack != m_Track || m_CachedHead != m_Head) {
		if (m_TrackWritten)
			FinishWrite(); // the head moved before leaving write mode
		BuildTrack();
	}
	return true;
}

u8 Iwm::NextNibble()
{
	if (!TrackReady())
		return 0x00; // no data ready
	const u8 b = m_TrackData[m_TrackPos];
	if (++m_TrackPos >= m_TrackLen)
		m_TrackPos = 0;
	// Count sectors as their data mark goes past (for diagnostics).
	m_Mark = (u32)((m_Mark << 8) | b) & 0xFFFFFF;
	if (m_Mark == 0xD5AAAD)
		++m_SectorsRead;
	return b;
}

void Iwm::WriteNibble(u8 value)
{
	if (!TrackReady())
		return;
	m_TrackData[m_TrackPos] = value;
	if (++m_TrackPos >= m_TrackLen)
		m_TrackPos = 0;
	m_TrackWritten = true;
}

void Iwm::FinishWrite()
{
	m_TrackWritten = false;
	if (!m_Image || m_CachedTrack < 0 || m_CachedHead >= m_Image->Sides())
		return;

	// Decode every intact data field on the track back into the image. The
	// track is a ring, so fields may wrap past the end of the buffer.
	const unsigned spt = gcr::SectorsPerTrack((unsigned)m_CachedTrack);
	const unsigned first = gcr::TrackStartBlock((unsigned)m_CachedTrack, m_Image->Sides()) +
			       m_CachedHead * spt;
	const unsigned len = m_TrackLen;
	const unsigned fieldNibbles = 3 + 1 + 703;
	u8 field[fieldNibbles];
	u8 sector[524], old[524];
	for (unsigned i = 0; i < len; ++i) {
		if (m_TrackData[i] != 0xD5 || m_TrackData[(i + 1) % len] != 0xAA ||
		    m_TrackData[(i + 2) % len] != 0xAD)
			continue;
		for (unsigned j = 0; j < fieldNibbles; ++j)
			field[j] = m_TrackData[(i + j) % len];
		const int s = gcr::Decode6(field[3]);
		if (s < 0 || (unsigned)s >= spt || !gcr::DecodeData(field + 4, sector)) {
			++m_BadWrites; // e.g. a field cut short by an overlapping write
			continue;
		}
		m_Image->ReadSector(first + (unsigned)s, old);
		if (memcmp(old, sector, sizeof sector) != 0) {
			m_Image->WriteSector(first + (unsigned)s, sector);
			++m_SectorsWritten;
		}
	}
}

u8 Iwm::Access(unsigned offset, bool write, u8 value, u64 now)
{
	const unsigned old = m_Lines;
	const unsigned line = 1u << ((offset >> 1) & 7);
	if (offset & 1)
		m_Lines |= line;
	else
		m_Lines &= ~line;
	if (old != m_Lines)
		LinesChanged(old);

	const bool q6 = (m_Lines & kQ6) != 0, q7 = (m_Lines & kQ7) != 0;
	const bool enable = (m_Lines & kEnable) != 0;

	if (write) {
		if (q6 && q7) {
			if (!enable)
				m_Mode = value & 0x1F;
			else
				WriteNibble(value);
		}
		return 0xFF;
	}

	if (!q7 && !q6) // data register
		return enable ? NextNibble() : 0xFF;
	if (!q7 && q6) // status register
		return (u8)((Sense(now) ? 0x80 : 0) | (enable ? 0x20 : 0) | (m_Mode & 0x1F));
	if (q7 && !q6) // write handshake: buffer ready, no underrun
		return 0xFF;
	return 0xFF;
}

} // namespace pimac
