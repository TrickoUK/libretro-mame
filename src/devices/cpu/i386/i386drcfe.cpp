// license:BSD-3-Clause
// copyright-holders:TrickoUK
/***************************************************************************

    i386 family UML recompiler front end

***************************************************************************/

#include "emu.h"
#include "i386drcfe.h"
#include "i386priv.h"

#include "cpu/drcfe.ipp"

#include <sstream>


namespace {

// longest instruction the CPU accepts
constexpr unsigned MAX_INSTRUCTION_BYTES = 15;

constexpr unsigned REPORT_LIMIT = 40;

// the bytes that follow an opcode
enum : u16
{
	F_MODRM = 1 << 0,
	F_REGONLY = 1 << 1,     // the mod field is ignored and the r/m field names a register
	F_IMM8 = 1 << 2,
	F_IMM16 = 1 << 3,
	F_IMMZ = 1 << 4,        // 16 or 32 bits, by operand size
	F_MOFFS = 1 << 5,       // 16 or 32 bits, by address size
	F_REL8 = 1 << 6,
	F_RELZ = 1 << 7,
	F_FARPTR = 1 << 8,      // offset by operand size, then a 16-bit selector
	F_GROUP3 = 1 << 9,      // test (/0 and /1) has an immediate operand
	F_INVALID = 1 << 10
};


constexpr u16 one_byte_format(u8 op)
{
	// the eight arithmetic operations share one layout
	if (op < 0x40)
	{
		switch (op & 7)
		{
		case 4:  return F_IMM8;
		case 5:  return F_IMMZ;
		case 6:
		case 7:  return 0;
		default: return F_MODRM;
		}
	}

	if (((op >= 0x84) && (op <= 0x8f)) || ((op >= 0xd0) && (op <= 0xd3)) || ((op >= 0xd8) && (op <= 0xdf)))
		return F_MODRM;
	if ((op >= 0x70) && (op <= 0x7f))
		return F_REL8;
	if ((op >= 0xa0) && (op <= 0xa3))
		return F_MOFFS;
	if ((op >= 0xb0) && (op <= 0xb7))
		return F_IMM8;
	if ((op >= 0xb8) && (op <= 0xbf))
		return F_IMMZ;
	if ((op >= 0xe0) && (op <= 0xe3))
		return F_REL8;
	if ((op >= 0xe4) && (op <= 0xe7))
		return F_IMM8;

	switch (op)
	{
	case 0x62:
	case 0x63:
	case 0xc4:
	case 0xc5:
	case 0xfe:
	case 0xff:
		return F_MODRM;
	case 0x68:
	case 0xa9:
		return F_IMMZ;
	case 0x69:
	case 0x81:
	case 0xc7:
		return F_MODRM | F_IMMZ;
	case 0x6a:
	case 0xa8:
	case 0xcd:
	case 0xd4:
	case 0xd5:
		return F_IMM8;
	case 0x6b:
	case 0x80:
	case 0x82:
	case 0x83:
	case 0xc0:
	case 0xc1:
	case 0xc6:
		return F_MODRM | F_IMM8;
	case 0x9a:
	case 0xea:
		return F_FARPTR;
	case 0xc2:
	case 0xca:
		return F_IMM16;
	case 0xc8:
		return F_IMM16 | F_IMM8;
	case 0xe8:
	case 0xe9:
		return F_RELZ;
	case 0xeb:
		return F_REL8;
	case 0xf6:
	case 0xf7:
		return F_MODRM | F_GROUP3;
	default:
		return 0;
	}
}


constexpr u16 two_byte_format(u8 op)
{
	if ((op >= 0x80) && (op <= 0x8f))
		return F_RELZ;
	if ((op >= 0xc8) && (op <= 0xcf))   // bswap
		return 0;

	switch (op)
	{
	case 0x04:
	case 0x0a:
	case 0x0c:
		return F_INVALID;
	case 0x05:
	case 0x06:
	case 0x07:
	case 0x08:
	case 0x09:
	case 0x0b:
	case 0x0e:
	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
	case 0x34:
	case 0x35:
	case 0x37:
	case 0x77:
	case 0xa0:
	case 0xa1:
	case 0xa2:
	case 0xa8:
	case 0xa9:
	case 0xaa:
		return 0;
	case 0x0f:  // 3DNow!, the operation follows the operands
		return F_MODRM | F_IMM8;
	case 0x20:
	case 0x21:
	case 0x22:
	case 0x23:
	case 0x24:
	case 0x26:
		return F_MODRM | F_REGONLY;
	case 0x70:
	case 0x71:
	case 0x72:
	case 0x73:
	case 0xa4:
	case 0xac:
	case 0xba:
	case 0xc2:
	case 0xc4:
	case 0xc5:
	case 0xc6:
		return F_MODRM | F_IMM8;
	default:
		return F_MODRM;
	}
}


// the bytes of one instruction, for the disassembler
class instruction_buffer : public util::disasm_interface::data_buffer
{
public:
	instruction_buffer(offs_t base, u8 const *bytes) : m_base(base), m_bytes(bytes) { }

	virtual u8 r8(offs_t pc) const override { return ((pc - m_base) < MAX_INSTRUCTION_BYTES) ? m_bytes[pc - m_base] : 0; }
	virtual u16 r16(offs_t pc) const override { return r8(pc) | (u16(r8(pc + 1)) << 8); }
	virtual u32 r32(offs_t pc) const override { return r16(pc) | (u32(r16(pc + 2)) << 16); }
	virtual u64 r64(offs_t pc) const override { return r32(pc) | (u64(r32(pc + 4)) << 32); }

private:
	offs_t const m_base;
	u8 const *const m_bytes;
};

} // anonymous namespace


i386_device::frontend::frontend(i386_device &cpu, u32 window_start, u32 window_end, u32 max_sequence)
	: drc_frontend_base<opcode_desc>(12, window_start, window_end, max_sequence)
	, m_cpu(cpu)
	, m_disassembler(&cpu)
{
}


i386_device::opcode_desc const *i386_device::frontend::describe_code(offs_t startpc)
{
	return do_describe_code(
			[this] (opcode_desc &desc, opcode_desc const *prev) { return describe(desc, prev); },
			startpc);
}


void i386_device::frontend::report() const
{
	if (m_check)
		osd_printf_info("%s: recompiler front end: %u instruction lengths compared with the disassembler, %u differ\n", m_cpu.tag(), m_checked, m_mismatched);
}


bool i386_device::frontend::describe(opcode_desc &desc, opcode_desc const *prev)
{
	u8 bytes[MAX_INSTRUCTION_BYTES] = { };
	unsigned length = 0;
	bool unreadable = false;
	bool overlong = false;

	// reads the next byte of the instruction, giving zero once a byte cannot be read
	auto const next =
			[this, &desc, &bytes, &length, &unreadable, &overlong] () -> u8
			{
				if (unreadable || overlong)
					return 0;
				if (length >= MAX_INSTRUCTION_BYTES)
				{
					overlong = true;
					return 0;
				}
				if (!m_cpu.drc_read_code(desc.pc + length, bytes[length]))
				{
					unreadable = true;
					return 0;
				}
				return bytes[length++];
			};
	auto const next_value =
			[&next] (unsigned count)
			{
				u32 value = 0;
				for (unsigned i = 0; i < count; i++)
					value |= u32(next()) << (i * 8);
				return value;
			};

	// prefixes
	u8 op = next();
	for (bool prefix = true; prefix && !unreadable && !overlong; )
	{
		switch (op)
		{
		case 0x26:  // ES, SS and DS have the same base and limit as the segment they replace
		case 0x36:
		case 0x3e:
			break;
		case 0x2e:
		case 0x64:
		case 0x65:
			desc.prefixes |= opcode_desc::PREFIX_SEGMENT;
			break;
		case 0x66:
			desc.prefixes |= opcode_desc::PREFIX_OPSIZE;
			break;
		case 0x67:
			desc.prefixes |= opcode_desc::PREFIX_ADSIZE;
			break;
		case 0xf0:
			desc.prefixes |= opcode_desc::PREFIX_LOCK;
			break;
		case 0xf2:
			desc.prefixes |= opcode_desc::PREFIX_REPNE;
			break;
		case 0xf3:
			desc.prefixes |= opcode_desc::PREFIX_REP;
			break;
		default:
			prefix = false;
			break;
		}
		if (prefix)
			op = next();
	}

	// opcode
	u16 format;
	if (op == 0x0f)
	{
		u8 const second = next();
		desc.opcode = 0x0f00 | second;
		if ((second == 0x38) || (second == 0x3a))
		{
			next();
			format = (second == 0x3a) ? (F_MODRM | F_IMM8) : F_MODRM;
		}
		else
		{
			format = two_byte_format(second);
		}
	}
	else
	{
		desc.opcode = op;
		format = one_byte_format(op);
	}

	// operands
	unsigned const operand_bytes = (desc.prefixes & opcode_desc::PREFIX_OPSIZE) ? 2 : 4;
	unsigned const address_bytes = (desc.prefixes & opcode_desc::PREFIX_ADSIZE) ? 2 : 4;
	if (format & F_MODRM)
	{
		desc.modrm = next();
		u8 const mod = BIT(desc.modrm, 6, 2);
		u8 const rm = BIT(desc.modrm, 0, 3);
		unsigned displacement_bytes = 0;
		if (!(format & F_REGONLY) && (mod != 3))
		{
			if (address_bytes == 2)
			{
				displacement_bytes = (mod == 1) ? 1 : ((mod == 2) || (rm == 6)) ? 2 : 0;
			}
			else
			{
				if (rm == 4)
					desc.sib = next();
				bool const absolute = !mod && ((rm == 5) || ((rm == 4) && (BIT(desc.sib, 0, 3) == 5)));
				displacement_bytes = (mod == 1) ? 1 : ((mod == 2) || absolute) ? 4 : 0;
			}
		}
		desc.displacement = next_value(displacement_bytes);
		if (displacement_bytes == 1)
			desc.displacement = s8(desc.displacement);

		if ((format & F_GROUP3) && (BIT(desc.modrm, 3, 3) <= 1))
			format |= (op == 0xf6) ? F_IMM8 : F_IMMZ;
	}
	if (format & F_MOFFS)
		desc.displacement = next_value(address_bytes);
	if (format & F_IMM16)
		desc.immediate = next_value(2);
	if (format & F_IMM8)
		desc.immediate = (format & F_IMM16) ? (desc.immediate | (u32(next()) << 16)) : next();
	if (format & F_IMMZ)
		desc.immediate = next_value(operand_bytes);
	if (format & F_FARPTR)
	{
		desc.immediate = next_value(operand_bytes);
		next_value(2);
	}
	if (format & F_REL8)
		desc.immediate = s8(next());
	if (format & F_RELZ)
		desc.immediate = (operand_bytes == 2) ? u32(s16(next_value(2))) : next_value(4);

	if (unreadable)
	{
		// the interpreter fetches the instruction when it is executed, and faults then if the address calls for it
		desc.length = length + 1;
		desc.prefixes = 0;
		desc.set_compiler_page_fault();
		desc.set_can_cause_exception();
		desc.set_end_sequence();
		desc.regin.set();
		desc.regout.set();
		return true;
	}

	desc.length = length;
	if (m_check)
		check_length(desc, bytes);

	if (overlong || (format & F_INVALID))
	{
		desc.set_invalid_opcode();
		desc.set_will_cause_exception();
		desc.set_end_sequence();
		desc.regin.set();
		desc.regout.set();
		return true;
	}

	// branch targets are relative to the following instruction
	if (format & (F_REL8 | F_RELZ))
		desc.targetpc = (operand_bytes == 2) ? BRANCH_TARGET_DYNAMIC : offs_t(desc.pc + length + desc.immediate);

	classify(desc);
	return true;
}


// fills in the family, register and flag use and control flow of a decoded instruction
void i386_device::frontend::classify(opcode_desc &desc)
{
	using family = opcode_desc::family;
	using lazy_flags = opcode_desc::lazy_flags;

	u16 const op = desc.opcode;
	u8 const mod = BIT(desc.modrm, 6, 2);
	u8 const reg = BIT(desc.modrm, 3, 3);
	u8 const rm = BIT(desc.modrm, 0, 3);

	// register use; a write to part of a register also depends on the rest of it
	auto const use_register =
			[&desc] (unsigned number, unsigned width, bool read, bool write)
			{
				unsigned const gpr = (width == 8) ? (number & 3) : number;
				if (read || (write && (width < 32)))
					desc.set_gpr_used(gpr);
				if (write)
					desc.set_gpr_modified(gpr);
			};
	auto const use_address =
			[&desc, mod, rm] ()
			{
				if (rm == 4)
				{
					u8 const base = BIT(desc.sib, 0, 3);
					u8 const index = BIT(desc.sib, 3, 3);
					if (mod || (base != 5))
						desc.set_gpr_used(base);
					if (index != 4)
						desc.set_gpr_used(index);
				}
				else if (mod || (rm != 5))
				{
					desc.set_gpr_used(rm);
				}
				desc.set_can_cause_exception();
			};
	auto const use_rm =
			[&desc, &use_register, &use_address, mod, rm] (unsigned width, bool read, bool write)
			{
				if (mod == 3)
					use_register(rm, width, read, write);
				else
					use_address();
			};
	auto const use_stack =
			[&desc] ()
			{
				desc.set_gpr_used(ESP);
				desc.set_gpr_modified(ESP);
				desc.set_can_cause_exception();
			};

	// arithmetic flags written, by the operation that produces them
	auto const set_flags =
			[&desc] (lazy_flags kind)
			{
				desc.flags_kind = kind;
				switch (kind)
				{
				case lazy_flags::ADC:
				case lazy_flags::SBB:
					desc.set_flag_used(opcode_desc::REG_BIT_CF);
					[[fallthrough]];
				case lazy_flags::ADD:
				case lazy_flags::SUB:
					desc.set_flag_modified(opcode_desc::REG_BIT_CF);
					desc.set_flag_modified(opcode_desc::REG_BIT_AF);
					break;
				case lazy_flags::LOGIC:
					desc.set_flag_modified(opcode_desc::REG_BIT_CF);
					break;
				case lazy_flags::INC:
				case lazy_flags::DEC:
					desc.set_flag_modified(opcode_desc::REG_BIT_AF);
					break;
				case lazy_flags::NONE:
					return;
				}
				desc.set_flag_modified(opcode_desc::REG_BIT_PF);
				desc.set_flag_modified(opcode_desc::REG_BIT_ZF);
				desc.set_flag_modified(opcode_desc::REG_BIT_SF);
				desc.set_flag_modified(opcode_desc::REG_BIT_OF);
			};

	// the condition of a jcc, from its low four bits
	auto const use_condition =
			[&desc] (unsigned condition)
			{
				switch (condition >> 1)
				{
				case 0: // o
					desc.set_flag_used(opcode_desc::REG_BIT_OF);
					break;
				case 1: // b
					desc.set_flag_used(opcode_desc::REG_BIT_CF);
					break;
				case 2: // z
					desc.set_flag_used(opcode_desc::REG_BIT_ZF);
					break;
				case 3: // be
					desc.set_flag_used(opcode_desc::REG_BIT_CF);
					desc.set_flag_used(opcode_desc::REG_BIT_ZF);
					break;
				case 4: // s
					desc.set_flag_used(opcode_desc::REG_BIT_SF);
					break;
				case 5: // p
					desc.set_flag_used(opcode_desc::REG_BIT_PF);
					break;
				case 6: // l
					desc.set_flag_used(opcode_desc::REG_BIT_SF);
					desc.set_flag_used(opcode_desc::REG_BIT_OF);
					break;
				case 7: // le
					desc.set_flag_used(opcode_desc::REG_BIT_ZF);
					desc.set_flag_used(opcode_desc::REG_BIT_SF);
					desc.set_flag_used(opcode_desc::REG_BIT_OF);
					break;
				}
			};

	// the eight operations of opcodes 0x00-0x3f and of group 1, in encoding order
	static constexpr lazy_flags ARITHMETIC[8] = {
			lazy_flags::ADD, lazy_flags::LOGIC, lazy_flags::ADC, lazy_flags::SBB,
			lazy_flags::LOGIC, lazy_flags::SUB, lazy_flags::LOGIC, lazy_flags::SUB };
	constexpr unsigned ARITHMETIC_CMP = 7;

	// control transfers, and instructions that can change the mode or the interrupt state
	auto const transfer =
			[&desc] ()
			{
				desc.set_is_unconditional_branch();
				desc.set_end_sequence();
			};

	auto const ends_block =
			[&desc] ()
			{
				desc.set_end_sequence();
			};

	if ((op < 0x40) && ((op & 7) < 6))
	{
		unsigned const operation = op >> 3;
		unsigned const width = (op & 1) ? 32 : 8;
		bool const writes = operation != ARITHMETIC_CMP;
		desc.kind = family::ALU;
		desc.width = width;
		set_flags(ARITHMETIC[operation]);
		switch (op & 7)
		{
		case 0:
		case 1:
			use_rm(width, true, writes);
			use_register(reg, width, true, false);
			break;
		case 2:
		case 3:
			use_register(reg, width, true, writes);
			use_rm(width, true, false);
			break;
		default:
			use_register(EAX, width, true, writes);
			break;
		}
	}
	else if ((op >= 0x40) && (op <= 0x4f))
	{
		desc.kind = family::INCDEC;
		set_flags((op < 0x48) ? lazy_flags::INC : lazy_flags::DEC);
		use_register(op & 7, 32, true, true);
	}
	else if ((op >= 0x50) && (op <= 0x5f))
	{
		desc.kind = family::PUSHPOP;
		use_register(op & 7, 32, op < 0x58, op >= 0x58);
		use_stack();
	}
	else if (((op >= 0x70) && (op <= 0x7f)) || ((op >= 0x0f80) && (op <= 0x0f8f)))
	{
		desc.kind = family::JCC;
		use_condition(op & 0xf);
		desc.set_is_conditional_branch();
	}
	else if ((op >= 0x91) && (op <= 0x97))
	{
		desc.kind = family::XCHG;
		use_register(EAX, 32, true, true);
		use_register(op & 7, 32, true, true);
	}
	else if ((op >= 0xb0) && (op <= 0xbf))
	{
		desc.kind = family::MOV;
		desc.width = (op < 0xb8) ? 8 : 32;
		use_register(op & 7, desc.width, false, true);
	}
	else if ((op >= 0xd8) && (op <= 0xdf))
	{
		desc.kind = family::X87;
		desc.set_flag_used(opcode_desc::REG_BIT_X87_SW);
		desc.set_flag_used(opcode_desc::REG_BIT_X87_CW);
		desc.set_flag_used(opcode_desc::REG_BIT_X87_TW);
		desc.set_flag_modified(opcode_desc::REG_BIT_X87_SW);
		desc.set_flag_modified(opcode_desc::REG_BIT_X87_TW);
		desc.set_can_cause_exception();
		if (mod != 3)
		{
			use_address();
			if ((op == 0xd9) && (reg == 5))             // fldcw
				desc.set_flag_modified(opcode_desc::REG_BIT_X87_CW);
		}
		else if ((op == 0xdf) && (desc.modrm == 0xe0))  // fnstsw ax
		{
			use_register(EAX, 16, false, true);
		}
		else if (((op == 0xdb) || (op == 0xdf)) && (desc.modrm >= 0xe8) && (desc.modrm <= 0xf7))    // fcomi, fucomi
		{
			desc.set_flag_modified(opcode_desc::REG_BIT_CF);
			desc.set_flag_modified(opcode_desc::REG_BIT_PF);
			desc.set_flag_modified(opcode_desc::REG_BIT_ZF);
		}
		else if (((op == 0xda) || (op == 0xdb)) && (desc.modrm <= 0xdf))    // fcmovcc
		{
			desc.set_flag_used(opcode_desc::REG_BIT_CF);
			desc.set_flag_used(opcode_desc::REG_BIT_PF);
			desc.set_flag_used(opcode_desc::REG_BIT_ZF);
		}
	}
	else switch (op)
	{
	case 0x68:  // push imm
	case 0x6a:
		desc.kind = family::PUSHPOP;
		use_stack();
		break;

	case 0x80:  // group 1
	case 0x81:
	case 0x82:
	case 0x83:
		desc.kind = family::ALU;
		desc.width = ((op == 0x80) || (op == 0x82)) ? 8 : 32;
		set_flags(ARITHMETIC[reg]);
		use_rm(desc.width, true, reg != ARITHMETIC_CMP);
		break;

	case 0x84:
	case 0x85:
		desc.kind = family::TEST;
		desc.width = (op == 0x84) ? 8 : 32;
		set_flags(lazy_flags::LOGIC);
		use_rm(desc.width, true, false);
		use_register(reg, desc.width, true, false);
		break;

	case 0xa8:
	case 0xa9:
		desc.kind = family::TEST;
		desc.width = (op == 0xa8) ? 8 : 32;
		set_flags(lazy_flags::LOGIC);
		use_register(EAX, desc.width, true, false);
		break;

	case 0x86:
	case 0x87:
		desc.kind = family::XCHG;
		desc.width = (op == 0x86) ? 8 : 32;
		use_rm(desc.width, true, true);
		use_register(reg, desc.width, true, true);
		break;

	case 0x88:
	case 0x89:
		desc.kind = family::MOV;
		desc.width = (op == 0x88) ? 8 : 32;
		use_register(reg, desc.width, true, false);
		use_rm(desc.width, false, true);
		break;

	case 0x8a:
	case 0x8b:
		desc.kind = family::MOV;
		desc.width = (op == 0x8a) ? 8 : 32;
		use_rm(desc.width, true, false);
		use_register(reg, desc.width, false, true);
		break;

	case 0x8d:  // lea, which does not access memory
		if (mod != 3)
		{
			desc.kind = family::LEA;
			use_address();
			use_register(reg, 32, false, true);
		}
		break;

	case 0x90:
		if (!(desc.prefixes & opcode_desc::PREFIX_REP))     // pause
			desc.kind = family::NOP;
		break;

	case 0xa0:
	case 0xa1:
	case 0xa2:
	case 0xa3:
		desc.kind = family::MOV;
		desc.width = (op & 1) ? 32 : 8;
		use_register(EAX, desc.width, op >= 0xa2, op < 0xa2);
		desc.set_can_cause_exception();
		break;

	case 0xc6:
	case 0xc7:
		if (!reg)
		{
			desc.kind = family::MOV;
			desc.width = (op == 0xc6) ? 8 : 32;
			use_rm(desc.width, false, true);
		}
		break;

	case 0xf6:
	case 0xf7:
		desc.width = (op == 0xf6) ? 8 : 32;
		if (!reg)
		{
			desc.kind = family::TEST;
			set_flags(lazy_flags::LOGIC);
			use_rm(desc.width, true, false);
		}
		else if (reg == 3)
		{
			desc.kind = family::ALU;
			set_flags(lazy_flags::SUB);
			use_rm(desc.width, true, true);
		}
		break;

	case 0xfe:
	case 0xff:
		desc.width = (op == 0xfe) ? 8 : 32;
		if (reg <= 1)
		{
			desc.kind = family::INCDEC;
			set_flags(reg ? lazy_flags::DEC : lazy_flags::INC);
			use_rm(desc.width, true, true);
		}
		else if (op == 0xff)
		{
			switch (reg)
			{
			case 2: // call r/m
				desc.kind = family::CALL;
				use_rm(32, true, false);
				use_stack();
				transfer();
				break;
			case 4: // jmp r/m
				desc.kind = family::JMP;
				use_rm(32, true, false);
				transfer();
				break;
			case 3: // far call and jmp
			case 5:
				transfer();
				break;
			case 6: // push r/m
				desc.kind = family::PUSHPOP;
				use_rm(32, true, false);
				use_stack();
				break;
			}
		}
		break;

	case 0x0fb6:
	case 0x0fb7:
	case 0x0fbe:
	case 0x0fbf:
		desc.kind = family::EXTEND;
		use_rm((op & 1) ? 16 : 8, true, false);
		use_register(reg, 32, false, true);
		break;

	case 0x0f31:
		desc.kind = family::RDTSC;
		use_register(EAX, 32, false, true);
		use_register(EDX, 32, false, true);
		break;

	case 0xc2:
	case 0xc3:
		desc.kind = family::RET;
		use_stack();
		transfer();
		break;

	case 0xe0:
	case 0xe1:
	case 0xe2:
	case 0xe3:
		desc.kind = family::LOOP;
		use_register(ECX, 32, true, op != 0xe3);
		if (op <= 0xe1)
			desc.set_flag_used(opcode_desc::REG_BIT_ZF);
		desc.set_is_conditional_branch();
		break;

	case 0xe8:
		desc.kind = family::CALL;
		use_stack();
		transfer();
		break;

	case 0xe9:
	case 0xeb:
		desc.kind = family::JMP;
		transfer();
		break;

	case 0x9a:      // call far
	case 0xca:      // retf
	case 0xcb:
	case 0xcc:      // int
	case 0xcd:
	case 0xce:
	case 0xcf:      // iret
	case 0xea:      // jmp far
	case 0xf1:
	case 0x0f05:    // syscall, loadall
	case 0x0f07:    // sysret
	case 0x0f34:    // sysenter
	case 0x0f35:    // sysexit
	case 0x0faa:    // rsm
		desc.set_can_cause_exception();
		transfer();
		break;

	case 0x07:      // pop es, ss, ds, fs, gs
	case 0x17:
	case 0x1f:
	case 0x0fa1:
	case 0x0fa9:
	case 0x8e:      // mov sreg
	case 0xc4:      // les, lds, lss, lfs, lgs
	case 0xc5:
	case 0x0fb2:
	case 0x0fb4:
	case 0x0fb5:
	case 0x9d:      // popf
	case 0xfa:      // cli
	case 0xfb:      // sti
	case 0xf4:      // hlt
	case 0x6c:      // port input and output
	case 0x6d:
	case 0x6e:
	case 0x6f:
	case 0xe4:
	case 0xe5:
	case 0xe6:
	case 0xe7:
	case 0xec:
	case 0xed:
	case 0xee:
	case 0xef:
	case 0x0f00:    // lldt, ltr and others
	case 0x0f01:    // lgdt, lidt, lmsw, invlpg
	case 0x0f06:    // clts
	case 0x0f08:    // invd, wbinvd
	case 0x0f09:
	case 0x0f20:    // control, debug and test registers
	case 0x0f21:
	case 0x0f22:
	case 0x0f23:
	case 0x0f24:
	case 0x0f26:
	case 0x0f30:    // wrmsr
		desc.set_can_cause_exception();
		ends_block();
		break;

	case 0x0f0b:    // ud2, ud1, ud0
	case 0x0fb9:
	case 0x0fff:
		desc.set_will_cause_exception();
		ends_block();
		break;
	}

	// 16-bit operand and address forms, FS, GS and CS overrides and repeated or locked instructions are left to the interpreter
	if (desc.prefixes)
	{
		desc.kind = family::STEP;
		desc.flags_kind = lazy_flags::NONE;
	}

	if (desc.kind == family::STEP)
	{
		desc.regin.set();
		desc.regout.set();
	}
}


void i386_device::frontend::check_length(opcode_desc const &desc, u8 const *bytes)
{
	instruction_buffer const buffer(desc.pc, bytes);
	std::ostringstream text;
	unsigned const length = m_disassembler.disassemble(text, desc.pc, buffer, buffer) & util::disasm_interface::LENGTHMASK;

	m_checked++;
	if (length != desc.length)
	{
		if (m_mismatched++ < REPORT_LIMIT)
		{
			osd_printf_warning(
					"%s: recompiler front end at %08X: length %u, disassembler %u (%s), bytes %02X %02X %02X %02X %02X %02X %02X %02X\n",
					m_cpu.tag(), desc.pc, desc.length, length, text.str(),
					bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
		}
	}
}
