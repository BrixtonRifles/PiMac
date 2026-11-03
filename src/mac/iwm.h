// SWIM floppy controller (IWM compatible) with two SuperDrives: internal
// (drive 0) and external (drive 1). The IWM picks the drive with its SELECT
// line, the ISM with mode bits 1 and 2.
//
// The SWIM starts up as an IWM, which reads and writes GCR (400K/800K)
// disks. Writing 1,0,1,1 to bit 6 of the IWM mode register switches it to the
// ISM, which reads and writes MFM (720K/1.44 MB) disks byte by byte through
// a two-byte FIFO; clearing bit 6 of the ISM mode register switches back.
// ROMs that know only the IWM (Plus, SE) never send that sequence, so they
// see an IWM as before.
//
// The drive works at the level the controller sees, like the real one, so
// the ROM's own .Sony driver runs unmodified: GCR nibbles in IWM mode, MFM
// bytes with "mark" flags in ISM mode. Timing is not emulated: the disk turns
// by one nibble each time the Mac reads the IWM data register, and by one byte
// each time it polls the ISM handshake register while reading. Data the Mac
// writes is collected in the track ring and decoded back into sectors when it
// leaves write mode.
//
// Drive registers (sense, selected by CA2 CA1 CA0 SEL) and commands (LSTRB)
// follow Inside Macintosh III, cross-checked with NetBSD's iwmreg.h, Clock
// Signal's Macintosh drive and MAME's SuperDrive; the ISM follows MAME's
// swim1.cpp and the Classic ROM's MFM code.
#pragma once

#include "core/compat.h"
#include "core/floppy_image.h"
#include "core/mfm.h"

namespace pimac {

class Iwm {
public:
	Iwm();

	void Reset(); // controller and drive state; the disk stays inserted

	// Controller space access: offset = address bits 9..12. `now` is the CPU
	// cycle count (for the tachometer).
	u8 Access(unsigned offset, bool write, u8 value, u64 now);
	void SetSel(bool sel); // VIA PA5 (SEL / head select)

	enum : unsigned { kInternal = 0, kExternal = 1, kDrives = 2 };

	// Not owned; the image must outlive the insertion.
	void Insert(unsigned drive, FloppyImage *image);
	void Remove(unsigned drive); // host-side removal (no Mac involvement)
	FloppyImage *Image(unsigned drive) const { return m_Drives[drive].image; }
	// True once after the Mac ejected the disk (the host should save it).
	bool TakeEjected(unsigned drive)
	{
		bool e = m_Drives[drive].ejected;
		m_Drives[drive].ejected = false;
		return e;
	}

	bool IsmMode() const { return (m_IsmMode & kIsmSelect) != 0; }
	bool DriveMfm(unsigned drive) const { return m_Drives[drive].mfm; }
	unsigned SectorsRead(unsigned drive) const { return m_Drives[drive].sectorsRead; }
	unsigned SectorsWritten(unsigned drive) const { return m_Drives[drive].sectorsWritten; }
	unsigned BadWrites(unsigned drive) const { return m_Drives[drive].badWrites; }

private:
	enum : unsigned {
		kCA0 = 1 << 0,
		kCA1 = 1 << 1,
		kCA2 = 1 << 2,
		kLSTRB = 1 << 3,
		kEnable = 1 << 4,
		kSelect = 1 << 5, // drive select: 0 internal, 1 external
		kQ6 = 1 << 6,
		kQ7 = 1 << 7,
	};
	// ISM mode register
	enum : u8 {
		kIsmClearFifo = 0x01,
		kIsmDrive1 = 0x02,
		kIsmDrive2 = 0x04,
		kIsmAction = 0x08,
		kIsmWrite = 0x10,
		kIsmHeadSel = 0x20,
		kIsmSelect = 0x40, // 1 = ISM, 0 = IWM
		kIsmMotorOn = 0x80,
	};
	static const unsigned kMaxTrack = mfm::kMaxTrackBytes;
	static const unsigned kIndexBytes = 16; // index pulse length, in MFM bytes

	// A SuperDrive and the disk in it.
	struct Drive {
		FloppyImage *image;
		int track;
		int stepDir; // +1 inward (higher tracks), -1 outward
		unsigned head;
		bool motorOn;
		bool mfm;      // in MFM mode
		bool switched; // disk inserted since the Mac last reset the flag
		bool ejected;

		// The current track and side as a ring one revolution long, built
		// from the image on demand: GCR nibbles, or MFM bytes plus mark
		// flags. Writes land in the ring at the head position (wrapping
		// round, as on a real disk) and are decoded back into the image
		// when the Mac leaves write mode.
		u8 data[kMaxTrack];
		u8 marks[(kMaxTrack + 7) / 8];
		unsigned len, pos;
		int cachedTrack;
		unsigned cachedHead;
		bool cachedMfm;
		bool written;
		u32 mark; // recent bytes, to count sectors as they pass

		unsigned sectorsRead, sectorsWritten, badWrites;
	};

	unsigned Phases() const; // CA0 CA1 CA2 LSTRB from the IWM or the ISM
	Drive *Selected();       // the drive the controller is talking to, or 0
	unsigned Register() const; // CA2 CA1 CA0 SEL as a 4-bit number
	bool Sense(u64 now);
	void Command();
	void PhasesChanged(unsigned old); // old Phases()
	Drive *Ready(); // the selected drive if it can read, track built
	u8 NextNibble();
	void WriteNibble(u8 value);
	static void BuildTrack(Drive &d);
	static void FinishWrite(Drive &d);
	void FinishWrites(); // both drives

	u8 IwmAccess(unsigned offset, bool write, u8 value, u64 now);
	u8 IsmRead(unsigned reg, u64 now);
	void IsmWrite(unsigned reg, u8 value);
	void IsmModeChanged(u8 old);
	void IsmStep(); // read mode: the disk turns by one byte
	bool IsmPop(u8 *value, bool *mark);
	void IsmPush(u8 value, bool mark, bool crcZero);
	void IsmFlush(); // write mode: FIFO to disk
	void IsmPut(unsigned token);

	unsigned m_Lines;
	bool m_Sel;
	u8 m_Mode;

	// ISM
	u8 m_IsmMode, m_IsmSetup, m_IsmPhases, m_IsmError;
	u8 m_IsmParams[16];
	unsigned m_IsmParamIndex;
	unsigned m_IwmToIsm; // progress through the 1,0,1,1 switch sequence
	struct FifoEntry {
		u16 token; // byte | kMarkToken | kCrcToken
		bool crcZero;
	};
	enum : unsigned { kMarkToken = 0x100, kCrcToken = 0x200 };
	FifoEntry m_Fifo[2];
	unsigned m_FifoCount;
	bool m_IsmSynced; // reading: a mark has been found
	u16 m_IsmCrc;

	Drive m_Drives[kDrives];
};

} // namespace pimac
