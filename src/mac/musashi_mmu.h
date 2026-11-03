/*
    PiMac replacement for Musashi's m68kmmu.h (installed over it by
    scripts/fetch-deps.sh; included into m68kcpu.c after m68kfpu.c, whose
    static READ_EA_* / WRITE_EA_* helpers it uses).

    Decodes the 68030 MMU instructions and hands them to PiMac's MMU
    (src/mac/mmu030.cpp). Address translation itself is done in PiMac's bus
    callbacks, so Musashi is built with M68K_EMULATE_PMMU off.
*/

enum {
	PIMAC_MMU_TC = 0, PIMAC_MMU_SRP = 1, PIMAC_MMU_CRP = 2,
	PIMAC_MMU_TT0 = 3, PIMAC_MMU_TT1 = 4, PIMAC_MMU_MMUSR = 5
};
void pimac_mmu_write_reg(int reg, unsigned long long value, int flushDisable);
unsigned long long pimac_mmu_read_reg(int reg);
void pimac_mmu_flush(void);
void pimac_mmu_load(unsigned logical, int supervisor); // PLOAD
unsigned pimac_mmu_test(unsigned logical, int write, int supervisor);

uint pmmu_translate_addr(uint addr_in)
{
	return addr_in;
}

int pimac_cpu_supervisor(void)
{
	return FLAG_S ? 1 : 0;
}

/* Address of a control-mode effective address. */
static uint pimac_mmu_control_ea(int ea)
{
	const int mode = (ea >> 3) & 7, reg = ea & 7;
	switch (mode) {
	case 2: return REG_A[reg];
	case 5: return REG_A[reg] + MAKE_INT_16(OPER_I_16());
	case 6: return m68ki_get_ea_ix(REG_A[reg]);
	case 7:
		if (reg == 0)
			return MAKE_INT_16(OPER_I_16());
		if (reg == 1) {
			uint hi = OPER_I_16();
			return (hi << 16) | OPER_I_16();
		}
		break;
	}
	return 0;
}

static int pimac_mmu_fc(uint modes)
{
	const uint f = modes & 0x1F;
	if (f & 0x10)
		return f & 7;
	if (f & 0x08)
		return REG_D[f & 7] & 7;
	return f ? REG_DFC & 7 : REG_SFC & 7;
}

void m68881_mmu_ops(void)
{
	const uint ir = m68ki_cpu.ir;
	const int ea = ir & 0x3f;

	if ((ir & 0xffc0) == 0xf0c0 || (ir & 0xffc0) == 0xf080 || ((ir >> 9) & 7) != 0) {
		m68ki_exception_1111(); /* PBcc etc.: 68851 only */
		return;
	}

	const uint modes = OPER_I_16();
	const uint top = (modes >> 13) & 7;

	if (top == 1) {
		/* PLOAD (001000x0000fffff), PFLUSHA (0x2400), PFLUSH (001mmm0mmmmfffff) */
		if ((modes & 0xfde0) == 0x2000) { /* PLOAD: enter the translation in the ATC */
			const uint at = pimac_mmu_control_ea(ea);
			pimac_mmu_load(at, (pimac_mmu_fc(modes) & 4) != 0);
			return;
		}
		if (((modes >> 10) & 7) == 6)
			pimac_mmu_control_ea(ea); /* PFLUSH fc,mask,<ea>: consume extension words */
		pimac_mmu_flush();
		return;
	}
	if (top == 4) { /* PTEST */
		const uint at = pimac_mmu_control_ea(ea);
		const int fc = pimac_mmu_fc(modes);
		const unsigned last = pimac_mmu_test(at, (modes & 0x200) ? 0 : 1, (fc & 4) != 0);
		if (modes & 0x100)
			REG_A[(modes >> 5) & 7] = last;
		return;
	}
	if (top == 0 || top == 2) { /* PMOVE: format 1 (TT0/TT1) or 2 (TC/SRP/CRP) */
		const uint preg = (modes >> 10) & 7;
		int reg = -1;
		if (top == 0)
			reg = preg == 2 ? PIMAC_MMU_TT0 : preg == 3 ? PIMAC_MMU_TT1 : -1;
		else
			reg = preg == 0 ? PIMAC_MMU_TC : preg == 2 ? PIMAC_MMU_SRP : preg == 3 ? PIMAC_MMU_CRP : -1;
		if (reg < 0) {
			m68ki_exception_illegal();
			return;
		}
		const int wide = reg == PIMAC_MMU_SRP || reg == PIMAC_MMU_CRP;
		if (modes & 0x200) { /* MMU -> memory */
			const unsigned long long v = pimac_mmu_read_reg(reg);
			if (wide)
				WRITE_EA_64(ea, v);
			else
				WRITE_EA_32(ea, (uint32)v);
		} else {
			const unsigned long long v = wide ? READ_EA_64(ea) : READ_EA_32(ea);
			pimac_mmu_write_reg(reg, v, (modes & 0x100) != 0); /* FD: keep the ATC */
		}
		return;
	}
	if (top == 3) { /* PMOVE MMUSR */
		if (modes & 0x200)
			WRITE_EA_16(ea, (uint16)pimac_mmu_read_reg(PIMAC_MMU_MMUSR));
		else
			pimac_mmu_write_reg(PIMAC_MMU_MMUSR, READ_EA_16(ea), 1);
		return;
	}
	m68ki_exception_illegal();
}
