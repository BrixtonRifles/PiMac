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
	RegHead0 = 0x8,     // RDDATA0 select lower head; MFM: 0 = index (CA2)
	RegHead1 = 0x9,     // RDDATA1 select upper head; MFM: 0 = index (CA2 SEL)
	RegSuperDrive = 0xA, //        1 = SuperDrive (reads MFM) (CA2 CA0)
	RegMfmMode = 0xB,   //         1 = drive in MFM mode    (CA2 CA0 SEL)
	RegSides = 0xC,     // SIDES   0 = single-sided drive   (CA2 CA1)
	RegReady = 0xD,     //         0 = ready                (CA2 CA1 SEL)
	RegInstalled = 0xE, // DRVIN   0 = drive installed      (CA2 CA1 CA0)
	RegDoubleDensity = 0xF, //     0 = HD disk or no disk (SuperDrive) (all)
};

// Drive commands, strobed with LSTRB; CA2 is the argument.
enum DriveCommand : unsigned {
	CmdDirection = 0x0,    // CA2: 0 inward, 1 outward
	CmdStep = 0x2,         // CA0
	CmdMotor = 0x4,        // CA1, CA2: 0 on, 1 off
	CmdEject = 0x6,        // CA1 CA0 (with CA2 set)
	CmdResetSwitched = 0x1, // SEL (with CA2 set)
	CmdEncoding = 0x3,     // CA0 SEL, CA2: 0 MFM, 1 GCR (SuperDrive)
};

Iwm::Iwm()
{
	for (unsigned i = 0; i < kDrives; ++i) {
		Drive &d = m_Drives[i];
		d.image = 0;
		d.ejected = false;
		d.sectorsRead = d.sectorsWritten = d.badWrites = 0;
	}
	Reset();
}

void Iwm::Reset()
{
	m_Lines = 0;
	m_Sel = false;
	m_Mode = 0;
	m_IsmMode = m_IsmSetup = m_IsmPhases = m_IsmError = 0;
	memset(m_IsmParams, 0, sizeof m_IsmParams);
	m_IsmParamIndex = 0;
	m_IwmToIsm = 0;
	m_FifoCount = 0;
	m_IsmSynced = false;
	m_IsmCrc = 0xFFFF;
	for (unsigned i = 0; i < kDrives; ++i) {
		Drive &d = m_Drives[i];
		d.track = 0;
		d.stepDir = 1;
		d.head = 0;
		d.motorOn = false;
		d.mfm = false;
		d.switched = d.image != 0;
		d.len = d.pos = 0;
		d.cachedTrack = -1;
		d.cachedHead = 0;
		d.cachedMfm = false;
		d.written = false;
		d.mark = 0;
	}
}

void Iwm::Insert(unsigned drive, FloppyImage *image)
{
	Drive &d = m_Drives[drive];
	d.image = image;
	d.switched = true;
	d.ejected = false;
	d.cachedTrack = -1;
}

void Iwm::Remove(unsigned drive)
{
	Drive &d = m_Drives[drive];
	if (d.written)
		FinishWrite(d);
	d.image = 0;
	d.cachedTrack = -1;
}

unsigned Iwm::Phases() const
{
	return (IsmMode() ? m_IsmPhases : m_Lines) & (kCA0 | kCA1 | kCA2 | kLSTRB);
}

Iwm::Drive *Iwm::Selected()
{
	if (!IsmMode())
		return &m_Drives[(m_Lines & kSelect) ? kExternal : kInternal];
	if (!(m_IsmMode & kIsmMotorOn))
		return 0;
	if (m_IsmMode & kIsmDrive1)
		return &m_Drives[kInternal];
	if (m_IsmMode & kIsmDrive2)
		return &m_Drives[kExternal];
	return 0;
}

unsigned Iwm::Register() const
{
	const unsigned p = Phases();
	return ((p & kCA2) ? 8u : 0u) | ((p & kCA1) ? 4u : 0u) | ((p & kCA0) ? 2u : 0u) |
	       (m_Sel ? 1u : 0u);
}

void Iwm::SetSel(bool sel)
{
	m_Sel = sel;
	const unsigned reg = Register();
	Drive *d = Selected();
	if (d && (reg == RegHead0 || reg == RegHead1))
		d->head = reg & 1;
}

bool Iwm::Sense(u64 now)
{
	Drive *d = Selected();
	if (!d)
		return true; // no drive: everything reads 1 ("not installed")

	switch (Register()) {
	case RegDirection:
		return d->stepDir < 0;
	case RegDiskIn:
		return d->image == 0;
	case RegStep:
		return true; // steps complete instantly
	case RegLocked:
		return d->image != 0 && d->image->Writable();
	case RegMotor:
		return !d->motorOn;
	case RegTrack0:
		return d->track != 0;
	case RegSwitched:
		return d->switched;
	case RegTach: {
		// 60 pulses per revolution. In GCR mode the drive spins faster on
		// the inner zones (394..590 rpm); in MFM mode it turns at 300 rpm.
		static const unsigned kRpm[5] = {394, 429, 472, 525, 590};
		const unsigned rpm = d->mfm ? 300 : kRpm[d->track >> 4];
		const u64 halfPeriod = 7833600ull / (rpm * 2); // 60 pulses/rev, rpm/60 rev/s
		return ((now / halfPeriod) & 1) != 0;
	}
	case RegHead0:
	case RegHead1:
		// In MFM mode these show the index pulse (the formatter waits for it).
		// The disk turns only as the Mac reads or writes, so the pulse is
		// the first few bytes of the track ring.
		if (!d->mfm)
			return false;
		if (!d->image || !d->motorOn)
			return true;
		Ready();
		return d->pos >= kIndexBytes;
	case RegSuperDrive:
		return true;
	case RegMfmMode:
		return d->mfm;
	case RegSides:
		return true; // double-sided drive
	case RegReady:
		return false;
	case RegInstalled:
		return false;
	case RegDoubleDensity:
		return d->image != 0 && !d->image->HighDensity();
	default:
		return true;
	}
}

void Iwm::Command()
{
	Drive *d = Selected();
	if (!d)
		return;
	const bool ca2 = (Phases() & kCA2) != 0;
	switch (Register() & 0x7) { // CA1 CA0 SEL
	case CmdDirection:
		d->stepDir = ca2 ? -1 : 1;
		break;
	case CmdStep:
		d->track += d->stepDir;
		if (d->track < 0)
			d->track = 0;
		if (d->track >= (int)gcr::kTracks)
			d->track = gcr::kTracks - 1;
		break;
	case CmdMotor:
		d->motorOn = !ca2;
		break;
	case CmdEject:
		if (ca2 && d->image) {
			if (d->written)
				FinishWrite(*d);
			d->image = 0;
			d->ejected = true;
			d->motorOn = false;
			d->cachedTrack = -1;
		}
		break;
	case CmdResetSwitched:
		if (ca2)
			d->switched = false;
		break;
	case CmdEncoding:
		if (d->written)
			FinishWrite(*d);
		d->mfm = !ca2;
		break;
	default:
		break;
	}
}

void Iwm::PhasesChanged(unsigned old)
{
	const unsigned now = Phases();
	if (((old ^ now) & kLSTRB) && (now & kLSTRB))
		Command();
	const unsigned reg = Register();
	Drive *d = Selected();
	if (d && (reg == RegHead0 || reg == RegHead1))
		d->head = reg & 1;
}

// Nibbles that pass the head in one revolution, per zone: the 800K drive
// spins at 394..590 rpm and data moves at 489.6 kbit/s.
static const unsigned kRingNibbles[5] = {9320, 8559, 7779, 6994, 6223};

void Iwm::BuildTrack(Drive &d)
{
	d.cachedTrack = d.track;
	d.cachedHead = d.head;
	d.cachedMfm = d.mfm;
	FloppyImage &image = *d.image;

	if (d.mfm) {
		d.len = mfm::TrackBytes(image.HighDensity());
		if (d.pos >= d.len)
			d.pos = 0;
		memset(d.marks, 0, mfm::MarkBytes(d.len));
		if (image.Mfm() && d.head < 2)
			mfm::BuildTrack(d.data, d.marks, d.len, (unsigned)d.track, d.head, image);
		else
			memset(d.data, 0x00, d.len); // nothing an ISM can read
		return;
	}

	d.len = kRingNibbles[d.track >> 4];
	if (d.pos >= d.len)
		d.pos = 0;
	memset(d.data, 0xFF, d.len);
	if (image.Mfm() || d.head >= image.Sides())
		return; // unformatted (for GCR): all sync

	const unsigned spt = gcr::SectorsPerTrack((unsigned)d.track);
	const unsigned first = gcr::TrackStartBlock((unsigned)d.track, image.Sides()) + d.head * spt;

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
	const unsigned gap = (d.len - spt * sectorNibbles) / spt;
	u8 sector[524];
	u8 *p = d.data;
	for (unsigned i = 0; i < spt; ++i) {
		const unsigned s = (unsigned)order[i];
		p += gap;
		gcr::EncodeAddress(p, (unsigned)d.track, s, d.head, image.FormatByte());
		p += gcr::kAddressNibbles + 6;
		image.ReadSector(first + s, sector);
		gcr::EncodeData(p, s, sector);
		p += gcr::kDataNibbles;
	}
}

Iwm::Drive *Iwm::Ready()
{
	Drive *d = Selected();
	if (!d || !d->image || !d->motorOn)
		return 0;
	if (d->cachedTrack != d->track || d->cachedHead != d->head || d->cachedMfm != d->mfm) {
		if (d->written)
			FinishWrite(*d); // the head moved before leaving write mode
		BuildTrack(*d);
	}
	return d;
}

u8 Iwm::NextNibble()
{
	Drive *d = Ready();
	if (!d)
		return 0x00; // no data ready
	const u8 b = d->data[d->pos];
	if (++d->pos >= d->len)
		d->pos = 0;
	// Count sectors as their data mark goes past (for diagnostics).
	d->mark = (u32)((d->mark << 8) | b) & 0xFFFFFF;
	if (d->mark == 0xD5AAAD)
		++d->sectorsRead;
	return b;
}

void Iwm::WriteNibble(u8 value)
{
	Drive *d = Ready();
	if (!d)
		return;
	d->data[d->pos] = value;
	if (d->cachedMfm)
		mfm::SetMark(d->marks, d->pos, false);
	if (++d->pos >= d->len)
		d->pos = 0;
	d->written = true;
}

void Iwm::FinishWrites()
{
	for (unsigned i = 0; i < kDrives; ++i)
		if (m_Drives[i].written)
			FinishWrite(m_Drives[i]);
}

void Iwm::FinishWrite(Drive &d)
{
	d.written = false;
	if (!d.image || d.cachedTrack < 0 || d.cachedHead >= d.image->Sides())
		return;
	FloppyImage &image = *d.image;

	if (d.cachedMfm) {
		if (!image.Mfm())
			return;
		const mfm::DecodeResult r = mfm::DecodeTrack(d.data, d.marks, d.len,
							     (unsigned)d.cachedTrack, d.cachedHead, image);
		d.sectorsWritten += r.written;
		d.badWrites += r.bad;
		return;
	}
	if (image.Mfm())
		return;

	// Decode every intact data field on the track back into the image. The
	// track is a ring, so fields may wrap past the end of the buffer.
	const unsigned spt = gcr::SectorsPerTrack((unsigned)d.cachedTrack);
	const unsigned first =
		gcr::TrackStartBlock((unsigned)d.cachedTrack, image.Sides()) + d.cachedHead * spt;
	const unsigned len = d.len;
	const unsigned fieldNibbles = 3 + 1 + 703;
	u8 field[fieldNibbles];
	u8 sector[524], old[524];
	for (unsigned i = 0; i < len; ++i) {
		if (d.data[i] != 0xD5 || d.data[(i + 1) % len] != 0xAA || d.data[(i + 2) % len] != 0xAD)
			continue;
		for (unsigned j = 0; j < fieldNibbles; ++j)
			field[j] = d.data[(i + j) % len];
		const int s = gcr::Decode6(field[3]);
		if (s < 0 || (unsigned)s >= spt || !gcr::DecodeData(field + 4, sector)) {
			++d.badWrites; // e.g. a field cut short by an overlapping write
			continue;
		}
		image.ReadSector(first + (unsigned)s, old);
		if (memcmp(old, sector, sizeof sector) != 0) {
			image.WriteSector(first + (unsigned)s, sector);
			++d.sectorsWritten;
		}
	}
}

u8 Iwm::Access(unsigned offset, bool write, u8 value, u64 now)
{
	if (IsmMode()) {
		if (write) {
			IsmWrite(offset & 7, value);
			return 0xFF;
		}
		return IsmRead(offset & 7, now);
	}
	return IwmAccess(offset, write, value, now);
}

u8 Iwm::IwmAccess(unsigned offset, bool write, u8 value, u64 now)
{
	const unsigned old = m_Lines;
	const unsigned line = 1u << ((offset >> 1) & 7);
	if (offset & 1)
		m_Lines |= line;
	else
		m_Lines &= ~line;
	if (old != m_Lines) {
		// Leaving write mode, or switching drives: decode what the Mac
		// wrote.
		if (((old & kQ7) && !(m_Lines & kQ7)) || ((old ^ m_Lines) & kSelect))
			FinishWrites();
		PhasesChanged(old & (kCA0 | kCA1 | kCA2 | kLSTRB));
	}

	const bool q6 = (m_Lines & kQ6) != 0, q7 = (m_Lines & kQ7) != 0;
	const bool enable = (m_Lines & kEnable) != 0;

	if (write) {
		if (q6 && q7) {
			if (!enable) {
				m_Mode = value & 0x1F;
				// The SWIM switches to the ISM when bit 6 of four mode
				// writes in a row goes 1, 0, 1, 1.
				const bool b6 = (value & 0x40) != 0;
				if (b6 != (m_IwmToIsm != 1))
					m_IwmToIsm = b6 ? 1 : 0;
				else if (m_IwmToIsm < 3)
					++m_IwmToIsm;
				else {
					m_IwmToIsm = 0;
					m_IsmMode |= kIsmSelect;
				}
			} else
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

// ISM registers: reads at offsets 8-15, writes at 0-7, both numbered 0-7.
enum IsmRegister : unsigned {
	IsmData = 0,
	IsmMark = 1,
	IsmCrcOrError = 2, // write: CRC, read: error
	IsmParams = 3,
	IsmPhase = 4,
	IsmSetup = 5,
	IsmModeZeros = 6, // read: mode (status)
	IsmModeOnes = 7,  // read: handshake
};

enum : u8 {
	ErrMarkAsData = 0x02, // a mark byte read through the data register
	ErrOverrun = 0x04,    // FIFO full on write, empty on read
};

bool Iwm::IsmPop(u8 *value, bool *mark)
{
	if (!m_FifoCount)
		return false;
	*value = (u8)m_Fifo[0].token;
	*mark = (m_Fifo[0].token & kMarkToken) != 0;
	m_Fifo[0] = m_Fifo[1];
	--m_FifoCount;
	return true;
}

void Iwm::IsmPush(u8 value, bool mark, bool crcZero)
{
	if (m_FifoCount >= 2)
		return;
	m_Fifo[m_FifoCount].token = (u16)(value | (mark ? kMarkToken : 0));
	m_Fifo[m_FifoCount].crcZero = crcZero;
	++m_FifoCount;
}

void Iwm::IsmStep()
{
	if (m_FifoCount >= 2)
		return;
	Drive *d = Ready();
	if (!d || !d->cachedMfm)
		return;
	const u8 b = d->data[d->pos];
	const bool mark = mfm::IsMark(d->marks, d->pos);
	if (++d->pos >= d->len)
		d->pos = 0;
	if (!m_IsmSynced) {
		if (!mark)
			return; // the ISM delivers nothing until it finds a mark
		m_IsmSynced = true;
	}
	m_IsmCrc = mark ? mfm::kCrcAfterMarks : mfm::Crc(m_IsmCrc, b);
	IsmPush(b, mark, m_IsmCrc == 0);
	if (!mark && b == 0xFB && d->mark)
		++d->sectorsRead;
	d->mark = mark;
}

void Iwm::IsmPut(unsigned token)
{
	if (m_FifoCount >= 2) {
		m_IsmError |= ErrOverrun;
		return;
	}
	m_Fifo[m_FifoCount].token = (u16)token;
	m_Fifo[m_FifoCount].crcZero = false;
	++m_FifoCount;
	if ((m_IsmMode & (kIsmAction | kIsmWrite)) == (kIsmAction | kIsmWrite))
		IsmFlush();
}

void Iwm::IsmFlush()
{
	while (m_FifoCount) {
		const unsigned token = m_Fifo[0].token;
		m_Fifo[0] = m_Fifo[1];
		--m_FifoCount;
		Drive *d = Ready();
		if (!d || !d->cachedMfm)
			continue;
		u8 bytes[2];
		unsigned n = 1;
		bool mark = false;
		if (token & kCrcToken) {
			bytes[0] = (u8)(m_IsmCrc >> 8);
			bytes[1] = (u8)m_IsmCrc;
			n = 2;
		} else {
			bytes[0] = (u8)token;
			mark = (token & kMarkToken) != 0;
		}
		for (unsigned i = 0; i < n; ++i) {
			d->data[d->pos] = bytes[i];
			mfm::SetMark(d->marks, d->pos, mark);
			if (++d->pos >= d->len)
				d->pos = 0;
			m_IsmCrc = mark ? mfm::kCrcAfterMarks : mfm::Crc(m_IsmCrc, bytes[i]);
		}
		d->written = true;
	}
}

void Iwm::IsmModeChanged(u8 old)
{
	const u8 rw = kIsmAction | kIsmWrite;
	const u8 now = m_IsmMode & rw, before = old & rw;
	if (now == rw && before != rw) {
		m_IsmCrc = 0xFFFF; // starting to write
		IsmFlush();
	} else if (before == rw && now != rw)
		FinishWrites();
	if (now == kIsmAction && before != kIsmAction)
		m_IsmSynced = false; // starting to read: look for a mark

	const u8 select = kIsmMotorOn | kIsmDrive1 | kIsmDrive2;
	if ((m_IsmMode ^ old) & select)
		FinishWrites(); // another drive

	if (!(m_IsmMode & kIsmSelect)) { // back to the IWM
		FinishWrites();
		m_IwmToIsm = 0;
	}
}

u8 Iwm::IsmRead(unsigned reg, u64 now)
{
	u8 value = 0xFF;
	bool mark = false;
	switch (reg) {
	case IsmData:
		if (!IsmPop(&value, &mark))
			m_IsmError |= ErrOverrun;
		else if (mark)
			m_IsmError |= ErrMarkAsData;
		return value;
	case IsmMark:
		if (!IsmPop(&value, &mark))
			m_IsmError |= ErrOverrun;
		return value;
	case IsmCrcOrError:
		value = m_IsmError;
		m_IsmError = 0;
		return value;
	case IsmParams:
		value = m_IsmParams[m_IsmParamIndex];
		m_IsmParamIndex = (m_IsmParamIndex + 1) & 15;
		return value;
	case IsmPhase:
		return m_IsmPhases;
	case IsmSetup:
		return m_IsmSetup;
	case IsmModeZeros:
		return m_IsmMode;
	default: { // handshake
		const bool writing = (m_IsmMode & kIsmWrite) != 0;
		if ((m_IsmMode & (kIsmAction | kIsmWrite)) == kIsmAction)
			IsmStep();
		u8 h = 0;
		if (m_FifoCount) {
			if (m_Fifo[0].token & kMarkToken)
				h |= 0x01;
			if (!m_Fifo[0].crcZero)
				h |= 0x02;
		}
		if (Sense(now))
			h |= 0x0C; // RDDATA and SENSE
		if (m_IsmError)
			h |= 0x20;
		if (writing)
			h |= m_FifoCount == 0 ? 0xC0 : m_FifoCount == 1 ? 0x80 : 0;
		else
			h |= m_FifoCount == 2 ? 0xC0 : m_FifoCount == 1 ? 0x80 : 0;
		return h;
	}
	}
}

void Iwm::IsmWrite(unsigned reg, u8 value)
{
	const u8 old = m_IsmMode;
	switch (reg) {
	case IsmData:
		IsmPut(value);
		break;
	case IsmMark:
		IsmPut(value | kMarkToken);
		break;
	case IsmCrcOrError:
		IsmPut(kCrcToken);
		break;
	case IsmParams:
		m_IsmParams[m_IsmParamIndex] = value;
		m_IsmParamIndex = (m_IsmParamIndex + 1) & 15;
		break;
	case IsmPhase: {
		const unsigned before = Phases();
		m_IsmPhases = value;
		PhasesChanged(before);
		break;
	}
	case IsmSetup:
		m_IsmSetup = value;
		break;
	case IsmModeZeros:
		m_IsmMode &= (u8)~value;
		m_IsmParamIndex = 0;
		IsmModeChanged(old);
		break;
	default: // mode ones
		m_IsmMode |= value;
		IsmModeChanged(old);
		break;
	}
	if (m_IsmMode & kIsmClearFifo)
		m_FifoCount = 0;
}

} // namespace pimac
