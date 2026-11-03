// IWM floppy controller with one internal Sony 800K drive.
//
// The drive works at the level of disk nibbles, like the real one, so the
// ROM's own .Sony driver runs unmodified. Timing is not emulated: each read
// of the data register returns the next nibble of the current track at once,
// which makes floppy access much faster than on a real Mac. Nibbles the Mac
// writes are collected and decoded back into sectors when it leaves write
// mode.
//
// Drive registers (sense, selected by CA2 CA1 CA0 SEL) and commands (LSTRB)
// follow Inside Macintosh III, cross-checked with NetBSD's iwmreg.h and Clock
// Signal's Macintosh drive.
#pragma once

#include "core/compat.h"
#include "core/floppy_image.h"

namespace pimac {

class Iwm {
public:
	Iwm();

	void Reset(); // controller and drive state; the disk stays inserted

	// IWM space access: offset = address bits 9..12. `now` is the CPU cycle
	// count (for the tachometer).
	u8 Access(unsigned offset, bool write, u8 value, u64 now);
	void SetSel(bool sel); // VIA PA5 (SEL / head select)

	void Insert(FloppyImage *image); // not owned; must outlive the insertion
	void Remove();                   // host-side removal (no Mac involvement)
	FloppyImage *Image() const { return m_Image; }
	// True once after the Mac ejected the disk (the host should save it).
	bool TakeEjected()
	{
		bool e = m_Ejected;
		m_Ejected = false;
		return e;
	}

	unsigned SectorsRead() const { return m_SectorsRead; }
	unsigned SectorsWritten() const { return m_SectorsWritten; }
	unsigned BadWrites() const { return m_BadWrites; }

private:
	enum : unsigned {
		kCA0 = 1 << 0,
		kCA1 = 1 << 1,
		kCA2 = 1 << 2,
		kLSTRB = 1 << 3,
		kEnable = 1 << 4,
		kExternal = 1 << 5, // drive select: 0 internal, 1 external
		kQ6 = 1 << 6,
		kQ7 = 1 << 7,
	};
	static const unsigned kMaxTrack = 9320; // nibbles per revolution, zone 0

	unsigned Register() const; // CA2 CA1 CA0 SEL as a 4-bit number
	bool Sense(u64 now);
	void Command();
	void LinesChanged(unsigned old);
	bool TrackReady(); // builds the track if needed; false if no data
	u8 NextNibble();
	void WriteNibble(u8 value);
	void BuildTrack();
	void FinishWrite();

	unsigned m_Lines;
	bool m_Sel;
	u8 m_Mode;

	// Drive
	FloppyImage *m_Image;
	int m_Track;
	int m_StepDir; // +1 inward (higher tracks), -1 outward
	unsigned m_Head;
	bool m_MotorOn;
	bool m_Switched; // disk inserted since the Mac last reset the flag
	bool m_Ejected;

	// The current track and side as a ring of nibbles one revolution long,
	// built from the image on demand. Writes land in the ring at the head
	// position (wrapping round, as on a real disk) and are decoded back into
	// the image when the Mac leaves write mode.
	u8 m_TrackData[kMaxTrack];
	unsigned m_TrackLen, m_TrackPos;
	int m_CachedTrack;
	unsigned m_CachedHead;

	bool m_TrackWritten;
	u32 m_Mark;

	unsigned m_SectorsRead, m_SectorsWritten, m_BadWrites;
};

} // namespace pimac
