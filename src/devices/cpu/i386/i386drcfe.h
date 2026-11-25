// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    i386 family UML recompiler front end

    Describes straight-line sequences of 32-bit code with flat segments
    (the only mode the recompiler compiles).  Each instruction is given its
    length, its family, the registers and flags it reads and writes, the
    way it produces its flags, and the information about control flow that
    decides where a block ends.

***************************************************************************/
#ifndef MAME_CPU_I386_I386DRCFE_H
#define MAME_CPU_I386_I386DRCFE_H

#pragma once

#include "i386.h"
#include "i386dasm.h"

#include "cpu/drcfe.h"


// description of one instruction
class i386_device::opcode_desc : public opcode_desc_base<opcode_desc, 32>
{
public:
	// bits of the register masks, the general registers first in the order of their encoding
	enum : unsigned
	{
		REG_BIT_GPR = 0,
		REG_BIT_CF = 8,
		REG_BIT_PF,
		REG_BIT_AF,
		REG_BIT_ZF,
		REG_BIT_SF,
		REG_BIT_OF,
		REG_BIT_DF,
		REG_BIT_X87_SW,     // status word, including the top of stack
		REG_BIT_X87_CW,
		REG_BIT_X87_TW,     // tag word and register stack
		REG_BIT_COUNT
	};

	// instruction families
	enum class family : u8
	{
		STEP,       // executed only by the interpreter's per-instruction function
		MOV,
		ALU,        // add, adc, and, cmp, or, sbb, sub, xor, neg
		TEST,
		INCDEC,
		LEA,
		EXTEND,     // movzx, movsx
		XCHG,
		NOP,
		PUSHPOP,
		JCC,
		LOOP,       // loop, loope, loopne, jecxz
		JMP,
		CALL,
		RET,
		X87,
		RDTSC
	};

	// the operation whose operands and result give the arithmetic flags, by width
	enum class lazy_flags : u8
	{
		NONE,       // the instruction leaves the arithmetic flags alone, or sets them in its own way
		ADD,
		ADC,
		SUB,        // also cmp and neg
		SBB,
		LOGIC,      // and, or, test, xor: CF and OF cleared, AF unchanged
		INC,        // CF unchanged
		DEC         // CF unchanged
	};

	// prefix bits
	enum : u8
	{
		PREFIX_OPSIZE = 1 << 0,
		PREFIX_ADSIZE = 1 << 1,
		PREFIX_LOCK = 1 << 2,
		PREFIX_REPNE = 1 << 3,
		PREFIX_REP = 1 << 4,
		PREFIX_SEGMENT = 1 << 5     // FS, GS or CS override
	};

	u16 opcode;             // primary opcode, 0x0fxx for the two-byte opcodes
	u8 modrm;
	u8 sib;
	u8 prefixes;
	u8 width;               // operand width in bits of a compiled instruction
	family kind;
	lazy_flags flags_kind;
	u32 displacement;
	u32 immediate;

	void set_gpr_used(unsigned n) { regin.set(REG_BIT_GPR + n); }
	void set_gpr_modified(unsigned n) { regout.set(REG_BIT_GPR + n); }
	void set_flag_used(unsigned bit) { regin.set(bit); }
	void set_flag_modified(unsigned bit) { regout.set(bit); }

	void reset(offs_t curpc, bool in_delay_slot)
	{
		opcode_desc_base<opcode_desc, 32>::reset(curpc, in_delay_slot);
		opcode = 0;
		modrm = 0;
		sib = 0;
		prefixes = 0;
		width = 32;
		kind = family::STEP;
		flags_kind = lazy_flags::NONE;
		displacement = 0;
		immediate = 0;
	}
};


class i386_device::frontend : public drc_frontend_base<opcode_desc>
{
public:
	frontend(i386_device &cpu, u32 window_start, u32 window_end, u32 max_sequence);

	// describes the code starting at a linear address; the first sequence of the list starts there
	opcode_desc const *describe_code(offs_t startpc);

	// compares each described length with the disassembler's, if asked to
	void set_check(bool value) { m_check = value; }
	void report() const;

private:
	bool describe(opcode_desc &desc, opcode_desc const *prev);
	void classify(opcode_desc &desc);
	void check_length(opcode_desc const &desc, u8 const *bytes);

	i386_device &m_cpu;
	i386_disassembler m_disassembler;
	bool m_check = false;
	u64 m_blocks = 0;
	u64 m_described = 0;
	u64 m_checked = 0;
	u64 m_mismatched = 0;
};

#endif // MAME_CPU_I386_I386DRCFE_H
