// Unit tests for the SE/30's 68882 (src/fpu/fpu68882.cpp) running under
// Musashi: small hand-assembled 68030 programs in a 64 KB RAM, checked
// against known results. Also checks the FPU probe (MOVES from CPU space)
// and that without the FPU, FPU opcodes still take the F-line exception.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>

extern "C" {
#include "m68k.h"
extern int pimac_fpu_present;
void pimac_fpu_reset(void);
}

namespace {

int g_Checks = 0;
int g_Failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                     \
		++g_Checks;                                                          \
		if (!(cond)) {                                                       \
			++g_Failures;                                                    \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
		}                                                                    \
	} while (0)

const uint32_t kRamSize = 0x10000;
uint8_t g_Ram[kRamSize];

const uint32_t kCode = 0x400;  // the program
const uint32_t kData = 0x1000; // A0 points here (results)
const uint32_t kIn1 = 0x1100;  // A1, A2: operands
const uint32_t kIn2 = 0x1110;

uint32_t Read(uint32_t a, unsigned n)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < n; ++i)
		v = (v << 8) | (a + i < kRamSize ? g_Ram[a + i] : 0xFF);
	return v;
}

void Write(uint32_t a, uint32_t v, unsigned n)
{
	for (unsigned i = 0; i < n; ++i)
		if (a + i < kRamSize)
			g_Ram[a + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}

void Put16(uint32_t &a, uint16_t v) { Write(a, v, 2), a += 2; }

// Assembles: MOVEA.L #kData,A0; MOVEA.L #kIn1,A1; MOVEA.L #kIn2,A2; the
// words; then BRA.S * (runs until the cycle budget is used).
void Load(std::initializer_list<uint16_t> words)
{
	memset(g_Ram, 0, sizeof g_Ram);
	Write(0, 0x8000, 4);  // SSP
	Write(4, kCode, 4);   // PC
	for (unsigned v = 2; v < 64; ++v)
		Write(v * 4, 0x320, 4); // anything else: D7 = -3
	Write(2 * 4, 0x300, 4);     // bus error: D7 = -1
	Write(11 * 4, 0x310, 4);    // F-line: D7 = -2
	uint32_t a = 0x300;
	Put16(a, 0x7EFF), Put16(a, 0x60FE);
	a = 0x310;
	Put16(a, 0x7EFE), Put16(a, 0x60FE);
	a = 0x320;
	Put16(a, 0x7EFD), Put16(a, 0x60FE);
	a = kCode;
	const uint16_t setup[] = {0x207C, 0x0000, (uint16_t)kData, 0x227C, 0x0000, (uint16_t)kIn1,
		0x247C, 0x0000, (uint16_t)kIn2};
	for (uint16_t w : setup)
		Put16(a, w);
	for (uint16_t w : words)
		Put16(a, w);
	Put16(a, 0x60FE);
}

void Run(bool fpu = true)
{
	m68k_set_cpu_type(M68K_CPU_TYPE_68030);
	pimac_fpu_present = fpu ? 1 : 0;
	pimac_fpu_reset();
	for (unsigned r = 0; r < 15; ++r) // D0-D7, A0-A6 (reset leaves them)
		m68k_set_reg((m68k_register_t)(M68K_REG_D0 + r), 0);
	m68k_pulse_reset();
	m68k_execute(20000);
}

uint32_t D(unsigned n) { return m68k_get_reg(0, (m68k_register_t)(M68K_REG_D0 + n)); }

double DoubleAt(uint32_t a)
{
	const uint64_t bits = ((uint64_t)Read(a, 4) << 32) | Read(a + 4, 4);
	double d;
	memcpy(&d, &bits, sizeof d);
	return d;
}

void PutExtended(uint32_t a, uint16_t signExp, uint64_t mantissa)
{
	Write(a, signExp, 2);
	Write(a + 2, 0, 2);
	Write(a + 4, (uint32_t)(mantissa >> 32), 4);
	Write(a + 8, (uint32_t)mantissa, 4);
}

bool Near(double got, double want, double tolerance = 4e-16)
{
	if (got == want)
		return true;
	const double scale = fabs(want) > 1 ? fabs(want) : 1;
	if (fabs(got - want) <= tolerance * scale)
		return true;
	printf("  got %.17g, want %.17g\n", got, want);
	return false;
}

// FPn = OPMODE(x) for x given as a double immediate; the result stored as a
// double at kData; FPSR in D0.
double Monadic(unsigned opmode, double x)
{
	uint64_t bits;
	memcpy(&bits, &x, sizeof bits);
	Load({0xF23C, (uint16_t)(0x5400 | opmode), // FOP.D #x,FP0
	      (uint16_t)(bits >> 48), (uint16_t)(bits >> 32), (uint16_t)(bits >> 16), (uint16_t)bits,
	      0xF210, 0x7400,                       // FMOVE.D FP0,(A0)
	      0xF200, 0xA800});                     // FMOVE.L FPSR,D0
	Run();
	return DoubleAt(kData);
}

void TestProbe()
{
	// MOVEQ #7,D0; MOVEC D0,SFC; MOVES.L $22000,D1 (the ROM's FPU probe);
	// MOVEQ #1,D2.
	const std::initializer_list<uint16_t> probe = {0x7007, 0x4E7B, 0x0000, 0x0EB9, 0x1000,
		0x0002, 0x2000, 0x7401};
	Load(probe);
	Run(true);
	CHECK(D(2) == 1 && D(7) == 0); // the 68882's interface registers answer
	Load(probe);
	Run(false);
	CHECK(D(2) == 0 && D(7) == 0xFFFFFFFF); // no FPU: bus error
	// Other CPU space addresses still bus-error.
	Load({0x7007, 0x4E7B, 0x0000, 0x0EB9, 0x1000, 0x0004, 0x2000, 0x7401});
	Run(true);
	CHECK(D(2) == 0 && D(7) == 0xFFFFFFFF);
}

void TestFline()
{
	// FNOP, then MOVEQ #1,D2.
	Load({0xF280, 0x0000, 0x7401});
	Run(false);
	CHECK(D(2) == 0 && D(7) == 0xFFFFFFFE); // no FPU: F-line
	Load({0xF280, 0x0000, 0x7401});
	Run(true);
	CHECK(D(2) == 1 && D(7) == 0);
	// Unimplemented encodings take the F-line exception, not a crash:
	// opclass 1 (undefined), and FMOVE.L FP0 to (d8,PC,Xn).
	Load({0xF200, 0x2000, 0x7401});
	Run(true);
	CHECK(D(2) == 0 && D(7) == 0xFFFFFFFE);
	Load({0xF23B, 0x6000, 0x0000, 0x7401});
	Run(true);
	CHECK(D(2) == 0 && D(7) == 0xFFFFFFFE);
}

void TestConstantsAndStore()
{
	// FMOVECR #0,FP0 (pi); FMOVE.X FP0,(A0)
	Load({0xF200, 0x5C00, 0xF210, 0x6800});
	Run();
	CHECK(Read(kData, 4) == 0x40000000 && Read(kData + 4, 4) == 0xC90FDAA2 &&
	      Read(kData + 8, 4) == 0x2168C235);
	// FMOVECR #$33 (10^1) -> FP1; FMOVE.L FP1,D1
	Load({0xF200, 0x5CB3, 0xF201, 0x6080});
	Run();
	CHECK(D(1) == 10);
	// An offset with no constant gives +0 (and sets Z).
	Load({0xF200, 0x5C20, 0xF200, 0xA800, 0xF201, 0x6000});
	Run();
	CHECK(D(1) == 0 && (D(0) & 0x04000000));
}

void TestArithmetic()
{
	CHECK(Near(Monadic(0x04, 2.0), sqrt(2.0)));            // FSQRT
	CHECK(Near(Monadic(0x18, -3.5), 3.5));                 // FABS
	CHECK(Near(Monadic(0x1A, 3.5), -3.5));                 // FNEG
	CHECK(Near(Monadic(0x01, 2.5), 2.0));                  // FINT (to nearest even)
	CHECK(Near(Monadic(0x03, -2.7), -2.0));                // FINTRZ
	CHECK(Near(Monadic(0x1E, 1000.0), 9.0));               // FGETEXP
	CHECK(Near(Monadic(0x1F, 1000.0), 1000.0 / 512));      // FGETMAN
	// FADD/FMUL/FDIV/FSUB with a long immediate: FP0 = 7; FP0 op= 2.
	struct { unsigned opmode; double want; } ops[] = {
		{0x22, 9}, {0x23, 14}, {0x20, 3.5}, {0x28, 5}};
	for (auto &op : ops) {
		Load({0xF23C, 0x4000, 0x0000, 0x0007, 0xF23C, (uint16_t)(0x4000 | op.opmode), 0x0000,
		      0x0002, 0xF210, 0x7400});
		Run();
		CHECK(Near(DoubleAt(kData), op.want));
	}
}

void TestTranscendental()
{
	const double pi = 3.14159265358979323846;
	CHECK(Near(Monadic(0x0E, pi / 6), 0.5, 1e-15));            // FSIN
	CHECK(Near(Monadic(0x1D, pi), -1.0));                      // FCOS
	CHECK(Near(Monadic(0x0F, pi / 4), 1.0, 1e-15));            // FTAN
	CHECK(Near(Monadic(0x0F, 1.2), tan(1.2), 1e-15));          // FTAN above 45 degrees
	CHECK(Near(Monadic(0x0F, 100.0), tan(100.0), 1e-13));      // FTAN, reduced argument
	CHECK(Near(Monadic(0x0A, 1.0), pi / 4));                   // FATAN
	CHECK(Near(Monadic(0x0C, 1.0), pi / 2));                   // FASIN
	CHECK(Near(Monadic(0x1C, -1.0), pi));                      // FACOS
	CHECK(Near(Monadic(0x10, 1.0), exp(1.0)));                 // FETOX
	CHECK(Near(Monadic(0x08, 1e-10), expm1(1e-10)));           // FETOXM1
	CHECK(Near(Monadic(0x11, 10.0), 1024.0));                  // FTWOTOX
	CHECK(Near(Monadic(0x12, 3.0), 1000.0));                   // FTENTOX
	CHECK(Near(Monadic(0x14, exp(2.0)), 2.0));                 // FLOGN
	CHECK(Near(Monadic(0x06, 1e-10), log1p(1e-10)));           // FLOGNP1
	CHECK(Near(Monadic(0x15, 1000.0), 3.0));                   // FLOG10
	CHECK(Near(Monadic(0x16, 1024.0), 10.0));                  // FLOG2
	CHECK(Near(Monadic(0x02, 1.0), sinh(1.0)));                // FSINH
	CHECK(Near(Monadic(0x19, 1.0), cosh(1.0)));                // FCOSH
	CHECK(Near(Monadic(0x09, 0.5), tanh(0.5)));                // FTANH
	CHECK(Near(Monadic(0x0D, 0.5), atanh(0.5)));               // FATANH
	// FSINCOS.D #x,FP1:FP0 (cos -> FP1, sin -> FP0): opmode $30 | 1
	uint64_t bits;
	const double x = 0.7;
	memcpy(&bits, &x, sizeof bits);
	Load({0xF23C, 0x5431, (uint16_t)(bits >> 48), (uint16_t)(bits >> 32), (uint16_t)(bits >> 16),
	      (uint16_t)bits, 0xF210, 0x7400, 0xF210, 0x7480, 0xF200, 0xA800});
	Run();
	CHECK(Near(DoubleAt(kData), cos(0.7)));                     // FP1, stored last
}

void TestRemainder()
{
	// FMOVE.L #10,FP0; FREM.L #3,FP0; FMOVE.X FP0,(A0); FMOVE.L FPSR,D0.
	// SANE's trigonometry needs FREM's quotient bits in the FPSR.
	Load({0xF23C, 0x4000, 0x0000, 0x000A, 0xF23C, 0x4025, 0x0000, 0x0003, 0xF210, 0x7400,
	      0xF200, 0xA800});
	Run();
	CHECK(Near(DoubleAt(kData), 1.0));
	CHECK(((D(0) >> 16) & 0xFF) == 3);
	// 11 REM 3: the quotient rounds to 4, remainder -1.
	Load({0xF23C, 0x4000, 0x0000, 0x000B, 0xF23C, 0x4025, 0x0000, 0x0003, 0xF210, 0x7400,
	      0xF200, 0xA800});
	Run();
	CHECK(Near(DoubleAt(kData), -1.0));
	CHECK(((D(0) >> 16) & 0xFF) == 4);
	// -11 MOD 3 (truncated): quotient 3 with the sign bit, remainder -2.
	Load({0xF23C, 0x4000, 0xFFFF, 0xFFF5, 0xF23C, 0x4021, 0x0000, 0x0003, 0xF210, 0x7400,
	      0xF200, 0xA800});
	Run();
	CHECK(Near(DoubleAt(kData), -2.0));
	CHECK(((D(0) >> 16) & 0xFF) == 0x83);
}

void TestNaNs()
{
	// 0/0: the 68882's default NaN (positive, all ones), OPERR, NaN bit.
	Load({0xF23C, 0x4000, 0x0000, 0x0000, 0xF23C, 0x4020, 0x0000, 0x0000, 0xF210, 0x6800,
	      0xF200, 0xA800});
	Run();
	CHECK(Read(kData, 2) == 0x7FFF && Read(kData + 4, 4) == 0xFFFFFFFF &&
	      Read(kData + 8, 4) == 0xFFFFFFFF);
	CHECK((D(0) & 0x01000000) && (D(0) & 0x2000));
	// NaN + NaN: the destination's NaN wins (the larger significand would on
	// an x87). FMOVE.X (A1),FP0; FADD.X (A2),FP0; FMOVE.X FP0,(A0)
	Load({0xF211, 0x4800, 0xF212, 0x4822, 0xF210, 0x6800});
	PutExtended(kIn1, 0x7FFF, 0xC000000000001111ull);
	PutExtended(kIn2, 0x7FFF, 0xC000000000009999ull);
	Run();
	CHECK(Read(kData, 2) == 0x7FFF && Read(kData + 8, 4) == 0x00001111);
	// A signalling NaN is quieted and sets SNAN.
	Load({0xF211, 0x4800, 0xF212, 0x4822, 0xF210, 0x6800, 0xF200, 0xA800});
	PutExtended(kIn1, 0x3FFF, 0x8000000000000000ull);
	PutExtended(kIn2, 0x7FFF, 0x8000000000002222ull);
	Run();
	CHECK(Read(kData + 4, 4) == 0xC0000000 && Read(kData + 8, 4) == 0x00002222);
	CHECK(D(0) & 0x4000);
}

void TestPacked()
{
	// FMOVE.P (A1),FP0 with 1.5E+0; FMOVE.D FP0,(A0)
	Load({0xF211, 0x4C00, 0xF210, 0x7400});
	Write(kIn1, 0x00000001, 4);
	Write(kIn1 + 4, 0x50000000, 4);
	Write(kIn1 + 8, 0x00000000, 4);
	Run();
	CHECK(Near(DoubleAt(kData), 1.5));
	// -2.5E-3
	Load({0xF211, 0x4C00, 0xF210, 0x7400});
	Write(kIn1, 0xC0030002, 4);
	Write(kIn1 + 4, 0x50000000, 4);
	Write(kIn1 + 8, 0x00000000, 4);
	Run();
	CHECK(Near(DoubleAt(kData), -2.5e-3));
	// FMOVECR pi; FMOVE.P FP0,(A0){#17}: 3.1415926535897932E+0
	Load({0xF200, 0x5C00, 0xF210, 0x6C11});
	Run();
	CHECK(Read(kData, 4) == 0x00000003 && Read(kData + 4, 4) == 0x14159265 &&
	      Read(kData + 8, 4) == 0x35897932);
}

void TestConditions()
{
	// FMOVE.L #3,FP0; FCMP.L #5,FP0; FBOLT.W over MOVEQ #1,D4; MOVEQ #1,D5;
	// FSOLT D6; FSOGT D3
	Load({0xF23C, 0x4000, 0x0000, 0x0003, 0xF23C, 0x4038, 0x0000, 0x0005, 0xF284, 0x0004,
	      0x7801, 0x7A01, 0xF246, 0x0004, 0xF243, 0x0002});
	Run();
	CHECK(D(4) == 0 && D(5) == 1);
	CHECK((D(6) & 0xFF) == 0xFF && (D(3) & 0xFF) == 0);
	// FDBcc: count D1 down from 3 while FP0 (= 1) is not equal to 0.
	// MOVEQ #3,D1; FMOVE.L #1,FP0; loop: ADDQ.L #1,D2; FDBEQ D1,loop
	Load({0x7203, 0xF23C, 0x4000, 0x0000, 0x0001, 0x5282, 0xF249, 0x0001, 0xFFFA});
	Run();
	CHECK(D(2) == 4 && (D(1) & 0xFFFF) == 0xFFFF);
	// Without the FPU, FDBcc is an F-line trap (Musashi's 68020/030
	// coprocessor entry for it used to do nothing).
	Load({0x7203, 0x5282, 0xF249, 0x0001, 0xFFFC});
	Run(false);
	CHECK(D(2) == 1 && D(7) == 0xFFFFFFFE);
	// FSOLT $1000.W (that entry's range also holds FScc to absolute
	// addresses): FMOVE.L #-1,FP0; FTST FP0; FSOLT $1000.W
	Load({0xF23C, 0x4000, 0xFFFF, 0xFFFF, 0xF200, 0x003A, 0xF278, 0x0004, 0x1000});
	Run();
	CHECK(Read(kData, 1) == 0xFF);
	// FTRAPcc: FTRAPNE (no operand) with FP0 = 0 continues; FTRAPEQ.W #0
	// takes the TRAPV vector (D7 = -3 via the catch-all).
	// FMOVE.L #0,FP0; FTRAPNE; MOVEQ #1,D2; FTRAPEQ.W #0; MOVEQ #1,D3
	Load({0xF23C, 0x4000, 0x0000, 0x0000, 0xF27C, 0x000E, 0x7401, 0xF27A, 0x0001, 0x0000,
	      0x7601});
	Run();
	CHECK(D(2) == 1 && D(3) == 0 && D(7) == 0xFFFFFFFD);
}

void TestSaveRestore()
{
	// Just reset: FSAVE writes a NULL frame (one longword of 0).
	// MOVEA.L A7,A3; FSAVE -(A7); MOVE.L A3,D1; SUB.L A7,D1; MOVE.L (A7),D2
	Load({0x264F, 0xF327, 0x220B, 0x928F, 0x2417});
	Run();
	CHECK(D(1) == 4 && D(2) == 0);
	// After an FPU instruction: the 68882's IDLE frame ($38 bytes + format
	// word), and FRESTORE (A7)+ pops it all.
	// FMOVE.L #1,FP0; ...; FRESTORE (A7)+; MOVE.L A7,D3; SUB.L A3,D3
	Load({0xF23C, 0x4000, 0x0000, 0x0001, 0x264F, 0xF327, 0x220B, 0x928F, 0x2417, 0xF35F,
	      0x260F, 0x968B});
	Run();
	CHECK(D(1) == 0x3C && D(2) == 0x1F380000 && D(3) == 0);
	// FRESTORE of a NULL frame resets the FPU: FP0 becomes a NaN.
	// FMOVE.L #1,FP0; CLR.L -(A7); FRESTORE (A7)+; FMOVE.X FP0,(A0)
	Load({0xF23C, 0x4000, 0x0000, 0x0001, 0x42A7, 0xF35F, 0xF210, 0x6800});
	Run();
	CHECK(Read(kData, 2) == 0x7FFF && Read(kData + 4, 4) == 0xFFFFFFFF);
	// FSAVE in user mode: privilege violation (D7 = -3 via the catch-all).
	// ANDI #$DFFF,SR; FSAVE -(A7)
	Load({0x027C, 0xDFFF, 0xF327, 0x7401});
	Run();
	CHECK(D(2) == 0 && D(7) == 0xFFFFFFFD);
}

void TestControlRegisters()
{
	// FMOVE.L #$00000020,FPCR (round toward minus infinity); FMOVE.L #1,FP0;
	// FDIV.L #3,FP0; FMOVE.L FPCR,D1; FMOVE.X FP0,(A0)
	Load({0xF23C, 0x9000, 0x0000, 0x0020, 0xF23C, 0x4000, 0x0000, 0x0001, 0xF23C, 0x4020,
	      0x0000, 0x0003, 0xF201, 0xB000, 0xF210, 0x6800});
	Run();
	CHECK(D(1) == 0x20);
	CHECK(Read(kData + 8, 4) == 0xAAAAAAAA); // 1/3 rounded down
	// FMOVEM.X FP0/FP1,-(A7) and back into FP2/FP3.
	// FMOVE.L #5,FP0; FMOVE.L #6,FP1; FMOVEM.X FP0-FP1,-(A7);
	// FMOVEM.X (A7)+,FP2-FP3; FMOVE.L FP2,D1; FMOVE.L FP3,D2
	Load({0xF23C, 0x4000, 0x0000, 0x0005, 0xF23C, 0x4080, 0x0000, 0x0006, 0xF227, 0xE003,
	      0xF21F, 0xD030, 0xF201, 0x6100, 0xF202, 0x6180});
	Run();
	CHECK(D(1) == 5 && D(2) == 6);
}

} // namespace

extern "C" {
// The bus: 64 KB of RAM, open bus (0xFF) above.
unsigned m68k_read_memory_8(unsigned a) { return Read(a, 1); }
unsigned m68k_read_memory_16(unsigned a) { return Read(a, 2); }
unsigned m68k_read_memory_32(unsigned a) { return Read(a, 4); }
unsigned m68k_read_immediate_16(unsigned a) { return Read(a, 2); }
unsigned m68k_read_immediate_32(unsigned a) { return Read(a, 4); }
unsigned m68k_read_pcrelative_8(unsigned a) { return Read(a, 1); }
unsigned m68k_read_pcrelative_16(unsigned a) { return Read(a, 2); }
unsigned m68k_read_pcrelative_32(unsigned a) { return Read(a, 4); }
void m68k_write_memory_8(unsigned a, unsigned v) { Write(a, v, 1); }
void m68k_write_memory_16(unsigned a, unsigned v) { Write(a, v, 2); }
void m68k_write_memory_32(unsigned a, unsigned v) { Write(a, v, 4); }
// PiMac's MMU hooks (src/mac/mmu030.h): unused here.
unsigned pimac_fault_address, pimac_fault_ssw, pimac_fault_data;
void pimac_mmu_write_reg(int, unsigned long long, int) {}
unsigned long long pimac_mmu_read_reg(int) { return 0; }
void pimac_mmu_flush(void) {}
void pimac_mmu_load(unsigned, int) {}
unsigned pimac_mmu_test(unsigned, int, int) { return 0; }
}

int main()
{
	m68k_init();
	TestProbe();
	TestFline();
	TestConstantsAndStore();
	TestArithmetic();
	TestTranscendental();
	TestRemainder();
	TestNaNs();
	TestPacked();
	TestConditions();
	TestSaveRestore();
	TestControlRegisters();
	printf("FPU: %d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? 1 : 0;
}
