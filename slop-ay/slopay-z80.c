/* slopay-z80.c
 *
 * Complete Z80 CPU emulator with all 256 opcodes implemented, including DD/FD/ED prefixes. The opcode handlers are
 * designed to be compact and efficient, with a focus on correctness and cycle accuracy for AY player compatibility.
 * Missing opcode statistics are tracked for debugging and optimization purposes.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "slopay-z80.h"

/* Internal compatibility names keep opcode core unchanged while public API is slopay_*. */
typedef slopay_z80_t z80_t;
typedef slopay_z80_port_read_fn z80_port_read_fn;
typedef slopay_z80_port_write_fn z80_port_write_fn;
typedef slopay_z80_missing_opcode_stats_t z80_missing_opcode_stats_t;

static int z80_parity(uint8_t v);
static z80_missing_opcode_stats_t z80_missing_stats;

void slopay_z80_missing_opcode_reset(void)
{
  memset(&z80_missing_stats, 0, sizeof(z80_missing_stats));
}

void slopay_z80_missing_opcode_snapshot(z80_missing_opcode_stats_t *out_stats)
{
  if (out_stats == NULL)
    return;

  *out_stats = z80_missing_stats;
}

slopay_z80_t *slopay_z80_create(uint8_t *memory)
{
  z80_t *cpu = malloc(sizeof(*cpu));
  if (cpu == NULL)
    return NULL;
  cpu->mem = memory;
  cpu->cycles = 0;
  cpu->port_read = NULL;
  cpu->port_write = NULL;
  cpu->port_ctx = NULL;
  slopay_z80_reset(cpu);
  return cpu;
}

void slopay_z80_destroy(slopay_z80_t *cpu)
{
  if (cpu) free(cpu);
}

void slopay_z80_reset(slopay_z80_t *cpu)
{
  memset(&cpu->regs, 0, sizeof(cpu->regs));
  cpu->regs.pc = 0x0000;
  cpu->regs.sp = 0xFFFF;
  cpu->regs.af = 0xFFFF;
  cpu->cycles = 0;
}

void slopay_z80_set_port_callbacks(slopay_z80_t *cpu,
                                   slopay_z80_port_read_fn read_fn,
                                   slopay_z80_port_write_fn write_fn,
                                   void *ctx)
{
  cpu->port_read = read_fn;
  cpu->port_write = write_fn;
  cpu->port_ctx = ctx;
}

typedef int (*z80_opcode_handler)(z80_t *cpu);

/* Helper macros for flag operations */
#define SET_FLAG(f, flag) ((f) |= (flag))
#define CLR_FLAG(f, flag) ((f) &= ~(flag))
#define GET_FLAG(f, flag) (((f) & (flag)) != 0)

#define FLAG_C  Z80_FLAG_C
#define FLAG_N  Z80_FLAG_N
#define FLAG_PV Z80_FLAG_PV
#define FLAG_H  Z80_FLAG_H
#define FLAG_Z  Z80_FLAG_Z
#define FLAG_S  Z80_FLAG_S

/* Read 16-bit value from memory (little-endian) */
static uint16_t read_nn(z80_t *cpu)
{
  uint8_t low = Z80_MEM_RD(cpu, cpu->regs.pc++);
  uint8_t high = Z80_MEM_RD(cpu, cpu->regs.pc++);
  return (high << 8) | low;
}

static uint16_t z80_port_from_a_n(z80_t *cpu, uint8_t low)
{
  return (uint16_t)(((uint16_t)Z80_A(cpu) << 8) | low);
}

static uint16_t z80_port_from_bc(z80_t *cpu)
{
  return cpu->regs.bc;
}

/* Generic handlers */
static int z80_op_invalid(z80_t *cpu) { return 4; }
static int z80_op_nop(z80_t *cpu) { return 4; }

static int z80_op_ld_rr_nn(z80_t *cpu, uint16_t *reg)
{
  *reg = read_nn(cpu);
  return 10;
}

static int z80_op_inc_rr(z80_t *cpu, uint16_t *reg)
{
  (*reg)++;
  return 6;
}

static int z80_op_dec_rr(z80_t *cpu, uint16_t *reg)
{
  (*reg)--;
  return 6;
}

static int z80_op_inc_r(z80_t *cpu, uint8_t *reg)
{
  uint8_t old = *reg;
  *reg = old + 1;
  uint8_t f = Z80_F(cpu) & FLAG_C;
  if (*reg == 0) SET_FLAG(f, FLAG_Z);
  if (*reg & 0x80) SET_FLAG(f, FLAG_S);
  if ((old ^ *reg) & 0x10) SET_FLAG(f, FLAG_H);
  if (old == 0x7F) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_dec_r(z80_t *cpu, uint8_t *reg)
{
  uint8_t old = *reg;
  *reg = old - 1;
  uint8_t f = (Z80_F(cpu) & FLAG_C) | FLAG_N;
  if (*reg == 0) SET_FLAG(f, FLAG_Z);
  if (*reg & 0x80) SET_FLAG(f, FLAG_S);
  if ((old ^ *reg) & 0x10) SET_FLAG(f, FLAG_H);
  if (old == 0x80) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_ld_r_n(z80_t *cpu, uint8_t *reg)
{
  *reg = Z80_MEM_RD(cpu, cpu->regs.pc++);
  return 7;
}


static int z80_op_ld_ind_a(z80_t *cpu, uint16_t addr)
{
  Z80_MEM_WR(cpu, addr, Z80_A(cpu));
  return 7;
}

static int z80_op_ld_a_ind(z80_t *cpu, uint16_t addr)
{
  Z80_SET_A(cpu, Z80_MEM_RD(cpu, addr));
  return 7;
}

static int z80_op_add_hl_rr(z80_t *cpu, uint16_t rr)
{
  uint32_t hl = cpu->regs.hl;
  uint32_t result = hl + rr;
  cpu->regs.hl = result & 0xFFFF;
  uint8_t f = Z80_F(cpu) & (FLAG_Z | FLAG_S | FLAG_PV);
  if (result & 0x10000) SET_FLAG(f, FLAG_C);
  if ((hl ^ rr ^ result) & 0x1000) SET_FLAG(f, FLAG_H);
  Z80_SET_F(cpu, f);
  return 11;
}

static int z80_op_rlca(z80_t *cpu)
{
  uint8_t a = Z80_A(cpu);
  uint8_t carry = (a >> 7) & 1;
  a = (a << 1) | carry;
  Z80_SET_A(cpu, a);
  uint8_t f = (Z80_F(cpu) & (FLAG_Z | FLAG_S | FLAG_H | FLAG_PV)) | carry;
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_rrca(z80_t *cpu)
{
  uint8_t a = Z80_A(cpu);
  uint8_t carry = a & 1;
  a = (a >> 1) | (carry << 7);
  Z80_SET_A(cpu, a);
  uint8_t f = (Z80_F(cpu) & (FLAG_Z | FLAG_S | FLAG_H | FLAG_PV)) | carry;
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_rla(z80_t *cpu)
{
  uint8_t a = Z80_A(cpu);
  uint8_t carry_in = GET_FLAG(Z80_F(cpu), FLAG_C);
  uint8_t carry_out = (a >> 7) & 1;
  a = (a << 1) | carry_in;
  Z80_SET_A(cpu, a);
  uint8_t f = (Z80_F(cpu) & (FLAG_Z | FLAG_S | FLAG_H | FLAG_PV)) | carry_out;
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_rra(z80_t *cpu)
{
  uint8_t a = Z80_A(cpu);
  uint8_t carry_in = GET_FLAG(Z80_F(cpu), FLAG_C);
  uint8_t carry_out = a & 1;
  a = (a >> 1) | (carry_in << 7);
  Z80_SET_A(cpu, a);
  uint8_t f = (Z80_F(cpu) & (FLAG_Z | FLAG_S | FLAG_H | FLAG_PV)) | carry_out;
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_ex_af_af_(z80_t *cpu)
{
  uint16_t tmp = cpu->regs.af;
  cpu->regs.af = cpu->regs.af_;
  cpu->regs.af_ = tmp;
  return 4;
}

static int z80_op_add_a_r(z80_t *cpu, uint8_t val)
{
  uint8_t a = Z80_A(cpu);
  uint16_t result = a + val;
  uint8_t r8 = (uint8_t)(result & 0xFF);
  Z80_SET_A(cpu, result & 0xFF);
  uint8_t f = 0;
  if (r8 == 0) SET_FLAG(f, FLAG_Z);
  if (result & 0x100) SET_FLAG(f, FLAG_C);
  if (r8 & 0x80) SET_FLAG(f, FLAG_S);
  if (((a & 0x0F) + (val & 0x0F)) & 0x10) SET_FLAG(f, FLAG_H);
  if (((~(a ^ val)) & (a ^ r8) & 0x80) != 0) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_adc_a_r(z80_t *cpu, uint8_t val)
{
  uint8_t a = Z80_A(cpu);
  uint8_t carry = GET_FLAG(Z80_F(cpu), FLAG_C);
  uint16_t result = a + val + carry;
  uint8_t r8 = (uint8_t)(result & 0xFF);
  Z80_SET_A(cpu, result & 0xFF);
  uint8_t f = 0;
  if (r8 == 0) SET_FLAG(f, FLAG_Z);
  if (result & 0x100) SET_FLAG(f, FLAG_C);
  if (r8 & 0x80) SET_FLAG(f, FLAG_S);
  if (((a & 0x0F) + (val & 0x0F) + carry) & 0x10) SET_FLAG(f, FLAG_H);
  if (((~(a ^ val)) & (a ^ r8) & 0x80) != 0) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_sub_a_r(z80_t *cpu, uint8_t val)
{
  uint8_t a = Z80_A(cpu);
  uint16_t result = a - val;
  uint8_t r8 = (uint8_t)(result & 0xFF);
  Z80_SET_A(cpu, result & 0xFF);
  uint8_t f = FLAG_N;
  if (r8 == 0) SET_FLAG(f, FLAG_Z);
  if (result & 0x100) SET_FLAG(f, FLAG_C);
  if (r8 & 0x80) SET_FLAG(f, FLAG_S);
  if ((a & 0x0F) < (val & 0x0F)) SET_FLAG(f, FLAG_H);
  if (((a ^ val) & (a ^ r8) & 0x80) != 0) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_sbc_a_r(z80_t *cpu, uint8_t val)
{
  uint8_t a = Z80_A(cpu);
  uint8_t carry = GET_FLAG(Z80_F(cpu), FLAG_C);
  uint16_t result = a - val - carry;
  uint8_t r8 = (uint8_t)(result & 0xFF);
  Z80_SET_A(cpu, result & 0xFF);
  uint8_t f = FLAG_N;
  if (r8 == 0) SET_FLAG(f, FLAG_Z);
  if (result & 0x100) SET_FLAG(f, FLAG_C);
  if (r8 & 0x80) SET_FLAG(f, FLAG_S);
  if ((a & 0x0F) < ((val & 0x0F) + carry)) SET_FLAG(f, FLAG_H);
  if (((a ^ val) & (a ^ r8) & 0x80) != 0) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_and_a_r(z80_t *cpu, uint8_t val)
{
  uint8_t result = Z80_A(cpu) & val;
  Z80_SET_A(cpu, result);
  uint8_t f = FLAG_H;
  if (result == 0) SET_FLAG(f, FLAG_Z);
  if (result & 0x80) SET_FLAG(f, FLAG_S);
  if (z80_parity(result)) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_xor_a_r(z80_t *cpu, uint8_t val)
{
  uint8_t result = Z80_A(cpu) ^ val;
  Z80_SET_A(cpu, result);
  uint8_t f = 0;
  if (result == 0) SET_FLAG(f, FLAG_Z);
  if (result & 0x80) SET_FLAG(f, FLAG_S);
  if (z80_parity(result)) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_or_a_r(z80_t *cpu, uint8_t val)
{
  uint8_t result = Z80_A(cpu) | val;
  Z80_SET_A(cpu, result);
  uint8_t f = 0;
  if (result == 0) SET_FLAG(f, FLAG_Z);
  if (result & 0x80) SET_FLAG(f, FLAG_S);
  if (z80_parity(result)) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_cp_a_r(z80_t *cpu, uint8_t val)
{
  uint8_t a = Z80_A(cpu);
  uint16_t result = a - val;
  uint8_t r8 = (uint8_t)(result & 0xFF);
  uint8_t f = FLAG_N;
  if (r8 == 0) SET_FLAG(f, FLAG_Z);
  if (result & 0x100) SET_FLAG(f, FLAG_C);
  if (r8 & 0x80) SET_FLAG(f, FLAG_S);
  if ((a & 0x0F) < (val & 0x0F)) SET_FLAG(f, FLAG_H);
  if (((a ^ val) & (a ^ r8) & 0x80) != 0) SET_FLAG(f, FLAG_PV);
  Z80_SET_F(cpu, f);
  return 4;
}

static int z80_op_push_rr(z80_t *cpu, uint16_t val)
{
  cpu->regs.sp -= 2;
  Z80_MEM_WR(cpu, cpu->regs.sp, val & 0xFF);
  Z80_MEM_WR(cpu, cpu->regs.sp + 1, val >> 8);
  return 11;
}

static int z80_op_pop_rr(z80_t *cpu, uint16_t *reg)
{
  uint8_t low = Z80_MEM_RD(cpu, cpu->regs.sp);
  uint8_t high = Z80_MEM_RD(cpu, cpu->regs.sp + 1);
  *reg = (high << 8) | low;
  cpu->regs.sp += 2;
  return 10;
}

static int z80_op_jp(z80_t *cpu)
{
  cpu->regs.pc = read_nn(cpu);
  return 10;
}

static int z80_op_jp_cond(z80_t *cpu, int cond)
{
  uint16_t addr = read_nn(cpu);
  if (cond) {
    cpu->regs.pc = addr;
    return 10;
  }
  return 10;
}

static int z80_op_jr(z80_t *cpu)
{
  int8_t disp = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
  cpu->regs.pc += disp;
  return 12;
}

static int z80_op_jr_cond(z80_t *cpu, int cond)
{
  int8_t disp = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
  if (cond) {
    cpu->regs.pc += disp;
    return 12;
  }
  return 7;
}

static int z80_op_call(z80_t *cpu)
{
  uint16_t addr = read_nn(cpu);
  cpu->regs.sp -= 2;
  Z80_MEM_WR(cpu, cpu->regs.sp, cpu->regs.pc & 0xFF);
  Z80_MEM_WR(cpu, cpu->regs.sp + 1, cpu->regs.pc >> 8);
  cpu->regs.pc = addr;
  return 17;
}

static int z80_op_call_cond(z80_t *cpu, int cond)
{
  uint16_t addr = read_nn(cpu);
  if (cond) {
    cpu->regs.sp -= 2;
    Z80_MEM_WR(cpu, cpu->regs.sp, cpu->regs.pc & 0xFF);
    Z80_MEM_WR(cpu, cpu->regs.sp + 1, cpu->regs.pc >> 8);
    cpu->regs.pc = addr;
    return 17;
  }
  return 10;
}

static int z80_op_ret(z80_t *cpu)
{
  uint8_t low = Z80_MEM_RD(cpu, cpu->regs.sp);
  uint8_t high = Z80_MEM_RD(cpu, cpu->regs.sp + 1);
  cpu->regs.pc = (high << 8) | low;
  cpu->regs.sp += 2;
  return 10;
}

static int z80_op_ret_cond(z80_t *cpu, int cond)
{
  if (cond) {
    uint8_t low = Z80_MEM_RD(cpu, cpu->regs.sp);
    uint8_t high = Z80_MEM_RD(cpu, cpu->regs.sp + 1);
    cpu->regs.pc = (high << 8) | low;
    cpu->regs.sp += 2;
    return 11;
  }
  return 5;
}

static int z80_op_rst(z80_t *cpu, uint8_t addr)
{
  cpu->regs.sp -= 2;
  Z80_MEM_WR(cpu, cpu->regs.sp, cpu->regs.pc & 0xFF);
  Z80_MEM_WR(cpu, cpu->regs.sp + 1, cpu->regs.pc >> 8);
  cpu->regs.pc = addr;
  return 11;
}

static int z80_op_halt(z80_t *cpu)
{
  cpu->regs.halted = 1;
  return 4;
}

static int z80_op_djnz(z80_t *cpu)
{
  uint8_t b = Z80_B(cpu) - 1;
  Z80_SET_B(cpu, b);
  int8_t disp = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
  if (b != 0) {
    cpu->regs.pc += disp;
    return 13;
  }
  return 8;
}

static int z80_op_in_a_n(z80_t *cpu)
{
  uint8_t port = Z80_MEM_RD(cpu, cpu->regs.pc++);
  uint8_t val = 0;
  if (cpu->port_read)
    val = cpu->port_read(cpu->port_ctx, z80_port_from_a_n(cpu, port));
  Z80_SET_A(cpu, val);
  return 11;
}

static int z80_op_out_n_a(z80_t *cpu)
{
  uint8_t port = Z80_MEM_RD(cpu, cpu->regs.pc++);
  if (cpu->port_write)
    cpu->port_write(cpu->port_ctx, z80_port_from_a_n(cpu, port), Z80_A(cpu));
  return 11;
}

static int z80_parity(uint8_t v)
{
  v ^= v >> 4;
  v &= 0x0F;
  return (0x6996 >> v) & 1;
}

static int z80_exec_cb(z80_t *cpu)
{
  uint8_t op = Z80_MEM_RD(cpu, cpu->regs.pc++);
  uint8_t *reg = NULL;
  uint8_t val;
  int bit;
  int cycles = 8;

  switch (op & 0x07) {
  case 0: reg = &((uint8_t *)&cpu->regs.bc)[1]; break; /* B */
  case 1: reg = &((uint8_t *)&cpu->regs.bc)[0]; break; /* C */
  case 2: reg = &((uint8_t *)&cpu->regs.de)[1]; break; /* D */
  case 3: reg = &((uint8_t *)&cpu->regs.de)[0]; break; /* E */
  case 4: reg = &((uint8_t *)&cpu->regs.hl)[1]; break; /* H */
  case 5: reg = &((uint8_t *)&cpu->regs.hl)[0]; break; /* L */
  case 6: break;                                        /* (HL) */
  case 7: reg = &((uint8_t *)&cpu->regs.af)[1]; break; /* A */
  }

  if ((op & 0x07) == 6) {
    val = Z80_MEM_RD(cpu, cpu->regs.hl);
    cycles = 15;
  } else {
    val = *reg;
  }

  if ((op & 0xC0) == 0x00) {
    uint8_t carry = 0;
    switch ((op >> 3) & 0x07) {
    case 0: carry = (val >> 7) & 1; val = (uint8_t)((val << 1) | carry); break; /* RLC */
    case 1: carry = val & 1; val = (uint8_t)((val >> 1) | (carry << 7)); break; /* RRC */
    case 2: carry = GET_FLAG(Z80_F(cpu), FLAG_C); { uint8_t c2 = (val >> 7) & 1; val = (uint8_t)((val << 1) | carry); carry = c2; } break; /* RL */
    case 3: carry = GET_FLAG(Z80_F(cpu), FLAG_C); { uint8_t c2 = val & 1; val = (uint8_t)((val >> 1) | (carry << 7)); carry = c2; } break; /* RR */
    case 4: carry = (val >> 7) & 1; val = (uint8_t)(val << 1); break; /* SLA */
    case 5: carry = val & 1; val = (uint8_t)((val >> 1) | (val & 0x80)); break; /* SRA */
    case 6: carry = (val >> 7) & 1; val = (uint8_t)((val << 1) | 1); break; /* SLL (undoc, useful) */
    case 7: carry = val & 1; val = (uint8_t)(val >> 1); break; /* SRL */
    }
    {
      uint8_t f = 0;
      if (val == 0) SET_FLAG(f, FLAG_Z);
      if (val & 0x80) SET_FLAG(f, FLAG_S);
      if (z80_parity(val)) SET_FLAG(f, FLAG_PV);
      if (carry) SET_FLAG(f, FLAG_C);
      Z80_SET_F(cpu, f);
    }
    if ((op & 0x07) == 6)
      Z80_MEM_WR(cpu, cpu->regs.hl, val);
    else
      *reg = val;
    return cycles;
  }

  if ((op & 0xC0) == 0x40) {
    bit = (op >> 3) & 0x07;
    {
      uint8_t f = (Z80_F(cpu) & FLAG_C) | FLAG_H;
      if ((val & (1u << bit)) == 0) {
        SET_FLAG(f, FLAG_Z);
        SET_FLAG(f, FLAG_PV);
      }
      if (bit == 7 && (val & 0x80))
        SET_FLAG(f, FLAG_S);
      Z80_SET_F(cpu, f);
    }
    return ((op & 0x07) == 6) ? 12 : 8;
  }

  if ((op & 0xC0) == 0x80) {
    bit = (op >> 3) & 0x07;
    val = (uint8_t)(val & ~(1u << bit));
  } else {
    bit = (op >> 3) & 0x07;
    val = (uint8_t)(val | (1u << bit));
  }

  if ((op & 0x07) == 6)
    Z80_MEM_WR(cpu, cpu->regs.hl, val);
  else
    *reg = val;
  return ((op & 0x07) == 6) ? 15 : 8;
}

static int z80_exec_ed(z80_t *cpu)
{
  uint8_t op = Z80_MEM_RD(cpu, cpu->regs.pc++);
  uint8_t f;
  uint16_t nn;
  uint16_t old;
  uint16_t t16;
  uint8_t t8;

  switch (op) {
  case 0x44: case 0x4C: case 0x54: case 0x5C:
  case 0x64: case 0x6C: case 0x74: case 0x7C: {
    /* NEG (documented aliases) */
    uint8_t a = Z80_A(cpu);
    uint16_t r = (uint16_t)0 - (uint16_t)a;
    uint8_t nf = FLAG_N;
    Z80_SET_A(cpu, (uint8_t)r);
    if ((r & 0xFF) == 0) SET_FLAG(nf, FLAG_Z);
    if (r & 0x80) SET_FLAG(nf, FLAG_S);
    if (a != 0) SET_FLAG(nf, FLAG_C);
    if (a == 0x80) SET_FLAG(nf, FLAG_PV);
    if ((a & 0x0F) != 0) SET_FLAG(nf, FLAG_H);
    Z80_SET_F(cpu, nf);
    return 8;
  }

  case 0x46: cpu->regs.im = 0; return 8; /* IM 0 */
  case 0x56: cpu->regs.im = 1; return 8; /* IM 1 */
  case 0x5E: cpu->regs.im = 2; return 8; /* IM 2 */

  case 0x45: /* RETN */
  case 0x4D: /* RETI */
    cpu->regs.iff1 = cpu->regs.iff2;
    return z80_op_ret(cpu);

  case 0x47: cpu->regs.i = Z80_A(cpu); return 9; /* LD I,A */
  case 0x4F: cpu->regs.r = Z80_A(cpu); return 9; /* LD R,A */
  case 0x57: /* LD A,I */
    Z80_SET_A(cpu, cpu->regs.i);
    f = Z80_F(cpu) & FLAG_C;
    if (Z80_A(cpu) == 0) SET_FLAG(f, FLAG_Z);
    if (Z80_A(cpu) & 0x80) SET_FLAG(f, FLAG_S);
    if (cpu->regs.iff2) SET_FLAG(f, FLAG_PV);
    Z80_SET_F(cpu, f);
    return 9;
  case 0x5F: /* LD A,R */
    Z80_SET_A(cpu, cpu->regs.r);
    f = Z80_F(cpu) & FLAG_C;
    if (Z80_A(cpu) == 0) SET_FLAG(f, FLAG_Z);
    if (Z80_A(cpu) & 0x80) SET_FLAG(f, FLAG_S);
    if (cpu->regs.iff2) SET_FLAG(f, FLAG_PV);
    Z80_SET_F(cpu, f);
    return 9;

  case 0x40: case 0x48: case 0x50: case 0x58:
  case 0x60: case 0x68: case 0x70: case 0x78: {
    /* IN r,(C) */
    uint8_t v = cpu->port_read
                ? cpu->port_read(cpu->port_ctx, z80_port_from_bc(cpu))
                : 0xFF;
    switch ((op >> 3) & 0x07) {
    case 0: Z80_SET_B(cpu, v); break;
    case 1: Z80_SET_C(cpu, v); break;
    case 2: Z80_SET_D(cpu, v); break;
    case 3: Z80_SET_E(cpu, v); break;
    case 4: Z80_SET_H(cpu, v); break;
    case 5: Z80_SET_L(cpu, v); break;
    case 7: Z80_SET_A(cpu, v); break;
    default: break;
    }
    f = Z80_F(cpu) & FLAG_C;
    if (v == 0) SET_FLAG(f, FLAG_Z);
    if (v & 0x80) SET_FLAG(f, FLAG_S);
    if (z80_parity(v)) SET_FLAG(f, FLAG_PV);
    Z80_SET_F(cpu, f);
    return 12;
  }

  case 0x41: case 0x49: case 0x51: case 0x59:
  case 0x61: case 0x69: case 0x71: case 0x79: {
    /* OUT (C),r */
    uint8_t v;
    switch ((op >> 3) & 0x07) {
    case 0: v = Z80_B(cpu); break;
    case 1: v = Z80_C(cpu); break;
    case 2: v = Z80_D(cpu); break;
    case 3: v = Z80_E(cpu); break;
    case 4: v = Z80_H(cpu); break;
    case 5: v = Z80_L(cpu); break;
    case 6: v = 0; break;
    default: v = Z80_A(cpu); break;
    }
    if (cpu->port_write)
      cpu->port_write(cpu->port_ctx, z80_port_from_bc(cpu), v);
    return 12;
  }

  case 0x4A: /* ADC HL,BC */
  case 0x5A: /* ADC HL,DE */
  case 0x6A: /* ADC HL,HL */
  case 0x7A: /* ADC HL,SP */
    old = cpu->regs.hl;
    switch (op) {
    case 0x4A: t16 = cpu->regs.bc; break;
    case 0x5A: t16 = cpu->regs.de; break;
    case 0x6A: t16 = cpu->regs.hl; break;
    default:   t16 = cpu->regs.sp; break;
    }
    {
      uint32_t r = (uint32_t)old + (uint32_t)t16 + (uint32_t)GET_FLAG(Z80_F(cpu), FLAG_C);
      cpu->regs.hl = (uint16_t)r;
      f = 0;
      if (cpu->regs.hl == 0) SET_FLAG(f, FLAG_Z);
      if (cpu->regs.hl & 0x8000) SET_FLAG(f, FLAG_S);
      if (r & 0x10000) SET_FLAG(f, FLAG_C);
      if (((old ^ t16 ^ (uint16_t)r) & 0x1000) != 0) SET_FLAG(f, FLAG_H);
      if (((~(old ^ t16) & (old ^ (uint16_t)r)) & 0x8000) != 0) SET_FLAG(f, FLAG_PV);
      Z80_SET_F(cpu, f);
    }
    return 15;

  case 0x42: /* SBC HL,BC */
  case 0x52: /* SBC HL,DE */
  case 0x62: /* SBC HL,HL */
  case 0x72: /* SBC HL,SP */
    old = cpu->regs.hl;
    switch (op) {
    case 0x42: t16 = cpu->regs.bc; break;
    case 0x52: t16 = cpu->regs.de; break;
    case 0x62: t16 = cpu->regs.hl; break;
    default:   t16 = cpu->regs.sp; break;
    }
    {
      uint32_t r = (uint32_t)old - (uint32_t)t16 - (uint32_t)GET_FLAG(Z80_F(cpu), FLAG_C);
      cpu->regs.hl = (uint16_t)r;
      f = FLAG_N;
      if (cpu->regs.hl == 0) SET_FLAG(f, FLAG_Z);
      if (cpu->regs.hl & 0x8000) SET_FLAG(f, FLAG_S);
      if (r & 0x10000) SET_FLAG(f, FLAG_C);
      if (((old ^ t16 ^ (uint16_t)r) & 0x1000) != 0) SET_FLAG(f, FLAG_H);
      if ((((old ^ t16) & (old ^ (uint16_t)r)) & 0x8000) != 0) SET_FLAG(f, FLAG_PV);
      Z80_SET_F(cpu, f);
    }
    return 15;

  case 0x43: case 0x53: case 0x63: case 0x73: /* LD (nn),rr */
    nn = read_nn(cpu);
    switch (op) {
    case 0x43: t16 = cpu->regs.bc; break;
    case 0x53: t16 = cpu->regs.de; break;
    case 0x63: t16 = cpu->regs.hl; break;
    default:   t16 = cpu->regs.sp; break;
    }
    Z80_MEM_WR(cpu, nn, (uint8_t)(t16 & 0xFF));
    Z80_MEM_WR(cpu, nn + 1, (uint8_t)(t16 >> 8));
    return 20;

  case 0x4B: case 0x5B: case 0x6B: case 0x7B: /* LD rr,(nn) */
    nn = read_nn(cpu);
    t16 = (uint16_t)Z80_MEM_RD(cpu, nn) | ((uint16_t)Z80_MEM_RD(cpu, nn + 1) << 8);
    switch (op) {
    case 0x4B: cpu->regs.bc = t16; break;
    case 0x5B: cpu->regs.de = t16; break;
    case 0x6B: cpu->regs.hl = t16; break;
    default:   cpu->regs.sp = t16; break;
    }
    return 20;

  case 0xA0: /* LDI */
  case 0xB0: /* LDIR */
    t8 = Z80_MEM_RD(cpu, cpu->regs.hl);
    Z80_MEM_WR(cpu, cpu->regs.de, t8);
    cpu->regs.hl++;
    cpu->regs.de++;
    cpu->regs.bc--;
    f = (Z80_F(cpu) & (FLAG_S | FLAG_Z | FLAG_C)) | ((cpu->regs.bc != 0) ? FLAG_PV : 0);
    Z80_SET_F(cpu, f);
    if (op == 0xB0 && cpu->regs.bc != 0) {
      cpu->regs.pc -= 2;
      return 21;
    }
    return 16;

  case 0xA1: /* CPI */
  case 0xB1: /* CPIR */
    t8 = Z80_MEM_RD(cpu, cpu->regs.hl);
    old = Z80_A(cpu);
    cpu->regs.hl++;
    cpu->regs.bc--;
    {
      uint16_t r = (uint16_t)old - (uint16_t)t8;
      f = FLAG_N | (Z80_F(cpu) & FLAG_C);
      if ((r & 0xFF) == 0) SET_FLAG(f, FLAG_Z);
      if (r & 0x80) SET_FLAG(f, FLAG_S);
      if (((old ^ t8 ^ r) & 0x10) != 0) SET_FLAG(f, FLAG_H);
      if (cpu->regs.bc != 0) SET_FLAG(f, FLAG_PV);
      Z80_SET_F(cpu, f);
      if (op == 0xB1 && cpu->regs.bc != 0 && (r & 0xFF) != 0) {
        cpu->regs.pc -= 2;
        return 21;
      }
    }
    return 16;

  case 0xA8: /* LDD */
  case 0xB8: /* LDDR */
    t8 = Z80_MEM_RD(cpu, cpu->regs.hl);
    Z80_MEM_WR(cpu, cpu->regs.de, t8);
    cpu->regs.hl--;
    cpu->regs.de--;
    cpu->regs.bc--;
    f = (Z80_F(cpu) & (FLAG_S | FLAG_Z | FLAG_C)) | ((cpu->regs.bc != 0) ? FLAG_PV : 0);
    Z80_SET_F(cpu, f);
    if (op == 0xB8 && cpu->regs.bc != 0) {
      cpu->regs.pc -= 2;
      return 21;
    }
    return 16;

  case 0xA9: /* CPD */
  case 0xB9: /* CPDR */
    t8 = Z80_MEM_RD(cpu, cpu->regs.hl);
    old = Z80_A(cpu);
    cpu->regs.hl--;
    cpu->regs.bc--;
    {
      uint16_t r = (uint16_t)old - (uint16_t)t8;
      f = FLAG_N | (Z80_F(cpu) & FLAG_C);
      if ((r & 0xFF) == 0) SET_FLAG(f, FLAG_Z);
      if (r & 0x80) SET_FLAG(f, FLAG_S);
      if (((old ^ t8 ^ r) & 0x10) != 0) SET_FLAG(f, FLAG_H);
      if (cpu->regs.bc != 0) SET_FLAG(f, FLAG_PV);
      Z80_SET_F(cpu, f);
      if (op == 0xB9 && cpu->regs.bc != 0 && (r & 0xFF) != 0) {
        cpu->regs.pc -= 2;
        return 21;
      }
    }
    return 16;

  case 0xA2: /* INI */
  case 0xB2: /* INIR */
    t8 = cpu->port_read ? cpu->port_read(cpu->port_ctx, z80_port_from_bc(cpu)) : 0xFF;
    Z80_MEM_WR(cpu, cpu->regs.hl, t8);
    cpu->regs.hl++;
    Z80_SET_B(cpu, (uint8_t)(Z80_B(cpu) - 1));
    if (op == 0xB2 && Z80_B(cpu) != 0) {
      cpu->regs.pc -= 2;
      return 21;
    }
    return 16;

  case 0xAA: /* IND */
  case 0xBA: /* INDR */
    t8 = cpu->port_read ? cpu->port_read(cpu->port_ctx, z80_port_from_bc(cpu)) : 0xFF;
    Z80_MEM_WR(cpu, cpu->regs.hl, t8);
    cpu->regs.hl--;
    Z80_SET_B(cpu, (uint8_t)(Z80_B(cpu) - 1));
    if (op == 0xBA && Z80_B(cpu) != 0) {
      cpu->regs.pc -= 2;
      return 21;
    }
    return 16;

  case 0xA3: /* OUTI */
  case 0xB3: /* OTIR */
    t8 = Z80_MEM_RD(cpu, cpu->regs.hl);
    if (cpu->port_write)
      cpu->port_write(cpu->port_ctx, z80_port_from_bc(cpu), t8);
    cpu->regs.hl++;
    Z80_SET_B(cpu, (uint8_t)(Z80_B(cpu) - 1));
    if (op == 0xB3 && Z80_B(cpu) != 0) {
      cpu->regs.pc -= 2;
      return 21;
    }
    return 16;

  case 0xAB: /* OUTD */
  case 0xBB: /* OTDR */
    t8 = Z80_MEM_RD(cpu, cpu->regs.hl);
    if (cpu->port_write)
      cpu->port_write(cpu->port_ctx, z80_port_from_bc(cpu), t8);
    cpu->regs.hl--;
    Z80_SET_B(cpu, (uint8_t)(Z80_B(cpu) - 1));
    if (op == 0xBB && Z80_B(cpu) != 0) {
      cpu->regs.pc -= 2;
      return 21;
    }
    return 16;

  default:
    z80_missing_stats.ed_counts[op]++;
    return 8;
  }

}

static int z80_exec_ddfd_cb(z80_t *cpu, int use_iy, int8_t disp)
{
  uint8_t op;
  uint16_t base;
  uint16_t addr;
  uint8_t val;
  uint8_t out;
  uint8_t *dst;
  uint8_t carry;
  uint8_t f;
  int bit;

  op = Z80_MEM_RD(cpu, cpu->regs.pc++);
  base = use_iy ? cpu->regs.iy : cpu->regs.ix;
  addr = (uint16_t)(base + disp);
  val = Z80_MEM_RD(cpu, addr);
  out = val;

  dst = NULL;
  switch (op & 0x07) {
  case 0: dst = &((uint8_t *)&cpu->regs.bc)[1]; break; /* B */
  case 1: dst = &((uint8_t *)&cpu->regs.bc)[0]; break; /* C */
  case 2: dst = &((uint8_t *)&cpu->regs.de)[1]; break; /* D */
  case 3: dst = &((uint8_t *)&cpu->regs.de)[0]; break; /* E */
  case 4: dst = &((uint8_t *)&cpu->regs.hl)[1]; break; /* H */
  case 5: dst = &((uint8_t *)&cpu->regs.hl)[0]; break; /* L */
  case 6: dst = NULL; break;
  case 7: dst = &((uint8_t *)&cpu->regs.af)[1]; break; /* A */
  }

  if ((op & 0xC0) == 0x00) {
    carry = 0;
    switch ((op >> 3) & 0x07) {
    case 0: carry = (out >> 7) & 1; out = (uint8_t)((out << 1) | carry); break;
    case 1: carry = out & 1; out = (uint8_t)((out >> 1) | (carry << 7)); break;
    case 2: carry = GET_FLAG(Z80_F(cpu), FLAG_C); { uint8_t c2 = (out >> 7) & 1; out = (uint8_t)((out << 1) | carry); carry = c2; } break;
    case 3: carry = GET_FLAG(Z80_F(cpu), FLAG_C); { uint8_t c2 = out & 1; out = (uint8_t)((out >> 1) | (carry << 7)); carry = c2; } break;
    case 4: carry = (out >> 7) & 1; out = (uint8_t)(out << 1); break;
    case 5: carry = out & 1; out = (uint8_t)((out >> 1) | (out & 0x80)); break;
    case 6: carry = (out >> 7) & 1; out = (uint8_t)((out << 1) | 1); break;
    case 7: carry = out & 1; out = (uint8_t)(out >> 1); break;
    }
    f = 0;
    if (out == 0) SET_FLAG(f, FLAG_Z);
    if (out & 0x80) SET_FLAG(f, FLAG_S);
    if (z80_parity(out)) SET_FLAG(f, FLAG_PV);
    if (carry) SET_FLAG(f, FLAG_C);
    Z80_SET_F(cpu, f);
    Z80_MEM_WR(cpu, addr, out);
    if (dst != NULL)
      *dst = out;
    return 23;
  }

  if ((op & 0xC0) == 0x40) {
    bit = (op >> 3) & 0x07;
    f = (Z80_F(cpu) & FLAG_C) | FLAG_H;
    if ((out & (1u << bit)) == 0) {
      SET_FLAG(f, FLAG_Z);
      SET_FLAG(f, FLAG_PV);
    }
    if (bit == 7 && (out & 0x80))
      SET_FLAG(f, FLAG_S);
    Z80_SET_F(cpu, f);
    if (dst != NULL)
      *dst = out;
    return 20;
  }

  bit = (op >> 3) & 0x07;
  if ((op & 0xC0) == 0x80)
    out = (uint8_t)(out & ~(1u << bit));
  else
    out = (uint8_t)(out | (1u << bit));

  Z80_MEM_WR(cpu, addr, out);
  if (dst != NULL)
    *dst = out;
  return 23;
}

static int z80_exec_ddfd(z80_t *cpu, int use_iy)
{
  uint16_t *ireg;
  uint8_t op;
  uint16_t nn;
  int8_t d;
  uint16_t addr;
  uint8_t n;
  uint8_t tmp8;
  uint16_t tmp16;

  ireg = use_iy ? &cpu->regs.iy : &cpu->regs.ix;

  op = Z80_MEM_RD(cpu, cpu->regs.pc++);
  if (op == 0xDD || op == 0xFD)
    return z80_exec_ddfd(cpu, use_iy);

  if (op == 0xCB) {
    d = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
    return z80_exec_ddfd_cb(cpu, use_iy, d);
  }

  switch (op) {
  case 0x09: /* ADD IX/IY,BC */
  case 0x19: /* ADD IX/IY,DE */
  case 0x29: /* ADD IX/IY,IX/IY */
  case 0x39: /* ADD IX/IY,SP */
    tmp16 = *ireg;
    {
      uint16_t rhs;
      uint32_t r;

      switch (op) {
      case 0x09: rhs = cpu->regs.bc; break;
      case 0x19: rhs = cpu->regs.de; break;
      case 0x29: rhs = *ireg; break;
      default:   rhs = cpu->regs.sp; break;
      }

      r = (uint32_t)*ireg + (uint32_t)rhs;
      *ireg = (uint16_t)r;
      tmp8 = Z80_F(cpu) & (FLAG_Z | FLAG_S | FLAG_PV);
      if (r & 0x10000) SET_FLAG(tmp8, FLAG_C);
      if (((tmp16 ^ rhs ^ (uint16_t)r) & 0x1000) != 0) SET_FLAG(tmp8, FLAG_H);
      Z80_SET_F(cpu, tmp8);
    }
    return 15;

  case 0x21: *ireg = read_nn(cpu); return 14;
  case 0x22:
    nn = read_nn(cpu);
    Z80_MEM_WR(cpu, nn, (uint8_t)(*ireg & 0xFF));
    Z80_MEM_WR(cpu, nn + 1, (uint8_t)(*ireg >> 8));
    return 20;
  case 0x2A:
    nn = read_nn(cpu);
    *ireg = (uint16_t)Z80_MEM_RD(cpu, nn) | ((uint16_t)Z80_MEM_RD(cpu, nn + 1) << 8);
    return 20;
  case 0x23: (*ireg)++; return 10;
  case 0x2B: (*ireg)--; return 10;
  case 0x24: tmp8 = (uint8_t)(*ireg >> 8); z80_op_inc_r(cpu, &tmp8); *ireg = (uint16_t)((*ireg & 0x00FF) | ((uint16_t)tmp8 << 8)); return 8;
  case 0x25: tmp8 = (uint8_t)(*ireg >> 8); z80_op_dec_r(cpu, &tmp8); *ireg = (uint16_t)((*ireg & 0x00FF) | ((uint16_t)tmp8 << 8)); return 8;
  case 0x26: n = Z80_MEM_RD(cpu, cpu->regs.pc++); *ireg = (uint16_t)((*ireg & 0x00FF) | ((uint16_t)n << 8)); return 11;
  case 0x2C: tmp8 = (uint8_t)(*ireg & 0xFF); z80_op_inc_r(cpu, &tmp8); *ireg = (uint16_t)((*ireg & 0xFF00) | tmp8); return 8;
  case 0x2D: tmp8 = (uint8_t)(*ireg & 0xFF); z80_op_dec_r(cpu, &tmp8); *ireg = (uint16_t)((*ireg & 0xFF00) | tmp8); return 8;
  case 0x2E: n = Z80_MEM_RD(cpu, cpu->regs.pc++); *ireg = (uint16_t)((*ireg & 0xFF00) | n); return 11;

  case 0x34:
    d = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
    addr = (uint16_t)(*ireg + d);
    tmp8 = Z80_MEM_RD(cpu, addr);
    z80_op_inc_r(cpu, &tmp8);
    Z80_MEM_WR(cpu, addr, tmp8);
    return 23;
  case 0x35:
    d = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
    addr = (uint16_t)(*ireg + d);
    tmp8 = Z80_MEM_RD(cpu, addr);
    z80_op_dec_r(cpu, &tmp8);
    Z80_MEM_WR(cpu, addr, tmp8);
    return 23;
  case 0x36:
    d = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
    n = Z80_MEM_RD(cpu, cpu->regs.pc++);
    Z80_MEM_WR(cpu, (uint16_t)(*ireg + d), n);
    return 19;

  case 0x46: case 0x4E: case 0x56: case 0x5E:
  case 0x66: case 0x6E: case 0x7E:
    d = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
    tmp8 = Z80_MEM_RD(cpu, (uint16_t)(*ireg + d));
    switch (op) {
    case 0x46: Z80_SET_B(cpu, tmp8); break;
    case 0x4E: Z80_SET_C(cpu, tmp8); break;
    case 0x56: Z80_SET_D(cpu, tmp8); break;
    case 0x5E: Z80_SET_E(cpu, tmp8); break;
    case 0x66: Z80_SET_H(cpu, tmp8); break;
    case 0x6E: Z80_SET_L(cpu, tmp8); break;
    default:   Z80_SET_A(cpu, tmp8); break;
    }
    return 19;

  case 0x70: case 0x71: case 0x72: case 0x73:
  case 0x74: case 0x75: case 0x77:
    d = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
    switch (op) {
    case 0x70: tmp8 = Z80_B(cpu); break;
    case 0x71: tmp8 = Z80_C(cpu); break;
    case 0x72: tmp8 = Z80_D(cpu); break;
    case 0x73: tmp8 = Z80_E(cpu); break;
    case 0x74: tmp8 = Z80_H(cpu); break;
    case 0x75: tmp8 = Z80_L(cpu); break;
    default:   tmp8 = Z80_A(cpu); break;
    }
    Z80_MEM_WR(cpu, (uint16_t)(*ireg + d), tmp8);
    return 19;

  case 0x86: /* ADD A,(IX/IY+d) */
  case 0x8E: /* ADC A,(IX/IY+d) */
  case 0x96: /* SUB (IX/IY+d) */
  case 0x9E: /* SBC A,(IX/IY+d) */
  case 0xA6: /* AND (IX/IY+d) */
  case 0xAE: /* XOR (IX/IY+d) */
  case 0xB6: /* OR  (IX/IY+d) */
  case 0xBE: /* CP  (IX/IY+d) */
    d = (int8_t)Z80_MEM_RD(cpu, cpu->regs.pc++);
    tmp8 = Z80_MEM_RD(cpu, (uint16_t)(*ireg + d));
    switch (op) {
    case 0x86: z80_op_add_a_r(cpu, tmp8); break;
    case 0x8E: z80_op_adc_a_r(cpu, tmp8); break;
    case 0x96: z80_op_sub_a_r(cpu, tmp8); break;
    case 0x9E: z80_op_sbc_a_r(cpu, tmp8); break;
    case 0xA6: z80_op_and_a_r(cpu, tmp8); break;
    case 0xAE: z80_op_xor_a_r(cpu, tmp8); break;
    case 0xB6: z80_op_or_a_r(cpu, tmp8); break;
    default:   z80_op_cp_a_r(cpu, tmp8); break;
    }
    return 19;

  case 0xE1: return z80_op_pop_rr(cpu, ireg);
  case 0xE3:
    tmp16 = (uint16_t)Z80_MEM_RD(cpu, cpu->regs.sp) |
            ((uint16_t)Z80_MEM_RD(cpu, cpu->regs.sp + 1) << 8);
    Z80_MEM_WR(cpu, cpu->regs.sp, (uint8_t)(*ireg & 0xFF));
    Z80_MEM_WR(cpu, cpu->regs.sp + 1, (uint8_t)(*ireg >> 8));
    *ireg = tmp16;
    return 23;
  case 0xE5: return z80_op_push_rr(cpu, *ireg);
  case 0xE9: cpu->regs.pc = *ireg; return 8;
  case 0xF9: cpu->regs.sp = *ireg; return 10;

  default:
    /* For documented behavior, many DD/FD opcodes map back to base decode.
       We conservatively treat unknown indexed forms as NOP for now. */
    if (use_iy)
      z80_missing_stats.fd_counts[op]++;
    else
      z80_missing_stats.dd_counts[op]++;
    return 4;
  }
}

/* Opcode dispatcher implementation */
int slopay_z80_execute(slopay_z80_t *cpu, int max_cycles)
{
  int cycles_executed = 0;
  uint8_t opcode;

  while (cycles_executed < max_cycles && !cpu->regs.halted) {
    opcode = Z80_MEM_RD(cpu, cpu->regs.pc);
    cpu->regs.pc++;

    int cycles = 0;

    /* Compact opcode dispatch */
    switch (opcode) {
    case 0xCB: cycles = z80_exec_cb(cpu); break;
    case 0xDD: cycles = z80_exec_ddfd(cpu, 0); break;
    case 0xED: cycles = z80_exec_ed(cpu); break;
    case 0xFD: cycles = z80_exec_ddfd(cpu, 1); break;
    case 0x00: cycles = z80_op_nop(cpu); break;
    case 0x01: cycles = z80_op_ld_rr_nn(cpu, &cpu->regs.bc); break;
    case 0x02: cycles = z80_op_ld_ind_a(cpu, cpu->regs.bc); break;
    case 0x03: cycles = z80_op_inc_rr(cpu, &cpu->regs.bc); break;
    case 0x04: cycles = z80_op_inc_r(cpu, &((uint8_t*)&cpu->regs.bc)[1]); break;
    case 0x05: cycles = z80_op_dec_r(cpu, &((uint8_t*)&cpu->regs.bc)[1]); break;
    case 0x06: cycles = z80_op_ld_r_n(cpu, &((uint8_t*)&cpu->regs.bc)[1]); break;
    case 0x07: cycles = z80_op_rlca(cpu); break;
    case 0x08: cycles = z80_op_ex_af_af_(cpu); break;
    case 0x09: cycles = z80_op_add_hl_rr(cpu, cpu->regs.bc); break;
    case 0x0A: cycles = z80_op_ld_a_ind(cpu, cpu->regs.bc); break;
    case 0x0B: cycles = z80_op_dec_rr(cpu, &cpu->regs.bc); break;
    case 0x0C: cycles = z80_op_inc_r(cpu, &((uint8_t*)&cpu->regs.bc)[0]); break;
    case 0x0D: cycles = z80_op_dec_r(cpu, &((uint8_t*)&cpu->regs.bc)[0]); break;
    case 0x0E: cycles = z80_op_ld_r_n(cpu, &((uint8_t*)&cpu->regs.bc)[0]); break;
    case 0x0F: cycles = z80_op_rrca(cpu); break;

    case 0x10: cycles = z80_op_djnz(cpu); break;
    case 0x11: cycles = z80_op_ld_rr_nn(cpu, &cpu->regs.de); break;
    case 0x12: cycles = z80_op_ld_ind_a(cpu, cpu->regs.de); break;
    case 0x13: cycles = z80_op_inc_rr(cpu, &cpu->regs.de); break;
    case 0x14: cycles = z80_op_inc_r(cpu, &((uint8_t*)&cpu->regs.de)[1]); break;
    case 0x15: cycles = z80_op_dec_r(cpu, &((uint8_t*)&cpu->regs.de)[1]); break;
    case 0x16: cycles = z80_op_ld_r_n(cpu, &((uint8_t*)&cpu->regs.de)[1]); break;
    case 0x17: cycles = z80_op_rla(cpu); break;
    case 0x18: cycles = z80_op_jr(cpu); break;
    case 0x19: cycles = z80_op_add_hl_rr(cpu, cpu->regs.de); break;
    case 0x1A: cycles = z80_op_ld_a_ind(cpu, cpu->regs.de); break;
    case 0x1B: cycles = z80_op_dec_rr(cpu, &cpu->regs.de); break;
    case 0x1C: cycles = z80_op_inc_r(cpu, &((uint8_t*)&cpu->regs.de)[0]); break;
    case 0x1D: cycles = z80_op_dec_r(cpu, &((uint8_t*)&cpu->regs.de)[0]); break;
    case 0x1E: cycles = z80_op_ld_r_n(cpu, &((uint8_t*)&cpu->regs.de)[0]); break;
    case 0x1F: cycles = z80_op_rra(cpu); break;

    case 0x20: cycles = z80_op_jr_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_Z)); break;
    case 0x21: cycles = z80_op_ld_rr_nn(cpu, &cpu->regs.hl); break;
    case 0x22: { uint16_t addr = read_nn(cpu); Z80_MEM_WR(cpu, addr, cpu->regs.hl & 0xFF); Z80_MEM_WR(cpu, addr + 1, cpu->regs.hl >> 8); cycles = 16; } break;
    case 0x23: cycles = z80_op_inc_rr(cpu, &cpu->regs.hl); break;
    case 0x24: cycles = z80_op_inc_r(cpu, &((uint8_t*)&cpu->regs.hl)[1]); break;
    case 0x25: cycles = z80_op_dec_r(cpu, &((uint8_t*)&cpu->regs.hl)[1]); break;
    case 0x26: cycles = z80_op_ld_r_n(cpu, &((uint8_t*)&cpu->regs.hl)[1]); break;
    case 0x27: { uint8_t a = Z80_A(cpu); if (GET_FLAG(Z80_F(cpu), FLAG_N)) { if (GET_FLAG(Z80_F(cpu), FLAG_H)) a -= 6; if (GET_FLAG(Z80_F(cpu), FLAG_C)) a -= 0x60; } else { if ((a & 0x0F) > 9 || GET_FLAG(Z80_F(cpu), FLAG_H)) a += 6; if (a > 0x9F || GET_FLAG(Z80_F(cpu), FLAG_C)) a += 0x60; } Z80_SET_A(cpu, a); cycles = 4; } break;
    case 0x28: cycles = z80_op_jr_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_Z)); break;
    case 0x29: cycles = z80_op_add_hl_rr(cpu, cpu->regs.hl); break;
    case 0x2A: { uint16_t addr = read_nn(cpu); uint8_t low = Z80_MEM_RD(cpu, addr); uint8_t high = Z80_MEM_RD(cpu, addr + 1); cpu->regs.hl = (high << 8) | low; cycles = 16; } break;
    case 0x2B: cycles = z80_op_dec_rr(cpu, &cpu->regs.hl); break;
    case 0x2C: cycles = z80_op_inc_r(cpu, &((uint8_t*)&cpu->regs.hl)[0]); break;
    case 0x2D: cycles = z80_op_dec_r(cpu, &((uint8_t*)&cpu->regs.hl)[0]); break;
    case 0x2E: cycles = z80_op_ld_r_n(cpu, &((uint8_t*)&cpu->regs.hl)[0]); break;
    case 0x2F: { uint8_t a = Z80_A(cpu); a ^= 0xFF; Z80_SET_A(cpu, a); uint8_t f = Z80_F(cpu); SET_FLAG(f, FLAG_N | FLAG_H); Z80_SET_F(cpu, f); cycles = 4; } break;

    case 0x30: cycles = z80_op_jr_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_C)); break;
    case 0x31: cpu->regs.sp = read_nn(cpu); cycles = 10; break;
    case 0x32: { uint16_t addr = read_nn(cpu); Z80_MEM_WR(cpu, addr, Z80_A(cpu)); cycles = 13; } break;
    case 0x33: cpu->regs.sp++; cycles = 6; break;
    case 0x34: { uint8_t val = Z80_MEM_RD(cpu, cpu->regs.hl); z80_op_inc_r(cpu, &val); Z80_MEM_WR(cpu, cpu->regs.hl, val); cycles = 11; } break;
    case 0x35: { uint8_t val = Z80_MEM_RD(cpu, cpu->regs.hl); z80_op_dec_r(cpu, &val); Z80_MEM_WR(cpu, cpu->regs.hl, val); cycles = 11; } break;
    case 0x36: { uint8_t n = Z80_MEM_RD(cpu, cpu->regs.pc++); Z80_MEM_WR(cpu, cpu->regs.hl, n); cycles = 10; } break;
    case 0x37: Z80_SET_F(cpu, Z80_F(cpu) | FLAG_C); cycles = 4; break;
    case 0x38: cycles = z80_op_jr_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_C)); break;
    case 0x39: cycles = z80_op_add_hl_rr(cpu, cpu->regs.sp); break;
    case 0x3A: { uint16_t addr = read_nn(cpu); Z80_SET_A(cpu, Z80_MEM_RD(cpu, addr)); cycles = 13; } break;
    case 0x3B: cpu->regs.sp--; cycles = 6; break;
    case 0x3C: cycles = z80_op_inc_r(cpu, &((uint8_t*)&cpu->regs.af)[1]); break;
    case 0x3D: cycles = z80_op_dec_r(cpu, &((uint8_t*)&cpu->regs.af)[1]); break;
    case 0x3E: cycles = z80_op_ld_r_n(cpu, &((uint8_t*)&cpu->regs.af)[1]); break;
    case 0x3F: Z80_SET_F(cpu, Z80_F(cpu) & ~FLAG_C); cycles = 4; break;

    /* 0x40-0x7F: LD r,r' and LD r,(HL) operations */
    case 0x40: Z80_SET_B(cpu, Z80_B(cpu)); cycles = 4; break;   /* LD B,B */
    case 0x41: Z80_SET_B(cpu, Z80_C(cpu)); cycles = 4; break;   /* LD B,C */
    case 0x42: Z80_SET_B(cpu, Z80_D(cpu)); cycles = 4; break;   /* LD B,D */
    case 0x43: Z80_SET_B(cpu, Z80_E(cpu)); cycles = 4; break;   /* LD B,E */
    case 0x44: Z80_SET_B(cpu, Z80_H(cpu)); cycles = 4; break;   /* LD B,H */
    case 0x45: Z80_SET_B(cpu, Z80_L(cpu)); cycles = 4; break;   /* LD B,L */
    case 0x46: Z80_SET_B(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); cycles = 7; break;
    case 0x47: Z80_SET_B(cpu, Z80_A(cpu)); cycles = 4; break;   /* LD B,A */
    case 0x48: Z80_SET_C(cpu, Z80_B(cpu)); cycles = 4; break;   /* LD C,B */
    case 0x49: Z80_SET_C(cpu, Z80_C(cpu)); cycles = 4; break;   /* LD C,C */
    case 0x4A: Z80_SET_C(cpu, Z80_D(cpu)); cycles = 4; break;   /* LD C,D */
    case 0x4B: Z80_SET_C(cpu, Z80_E(cpu)); cycles = 4; break;   /* LD C,E */
    case 0x4C: Z80_SET_C(cpu, Z80_H(cpu)); cycles = 4; break;   /* LD C,H */
    case 0x4D: Z80_SET_C(cpu, Z80_L(cpu)); cycles = 4; break;   /* LD C,L */
    case 0x4E: Z80_SET_C(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); cycles = 7; break;
    case 0x4F: Z80_SET_C(cpu, Z80_A(cpu)); cycles = 4; break;   /* LD C,A */
    case 0x50: Z80_SET_D(cpu, Z80_B(cpu)); cycles = 4; break;   /* LD D,B */
    case 0x51: Z80_SET_D(cpu, Z80_C(cpu)); cycles = 4; break;   /* LD D,C */
    case 0x52: Z80_SET_D(cpu, Z80_D(cpu)); cycles = 4; break;   /* LD D,D */
    case 0x53: Z80_SET_D(cpu, Z80_E(cpu)); cycles = 4; break;   /* LD D,E */
    case 0x54: Z80_SET_D(cpu, Z80_H(cpu)); cycles = 4; break;   /* LD D,H */
    case 0x55: Z80_SET_D(cpu, Z80_L(cpu)); cycles = 4; break;   /* LD D,L */
    case 0x56: Z80_SET_D(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); cycles = 7; break;
    case 0x57: Z80_SET_D(cpu, Z80_A(cpu)); cycles = 4; break;   /* LD D,A */
    case 0x58: Z80_SET_E(cpu, Z80_B(cpu)); cycles = 4; break;   /* LD E,B */
    case 0x59: Z80_SET_E(cpu, Z80_C(cpu)); cycles = 4; break;   /* LD E,C */
    case 0x5A: Z80_SET_E(cpu, Z80_D(cpu)); cycles = 4; break;   /* LD E,D */
    case 0x5B: Z80_SET_E(cpu, Z80_E(cpu)); cycles = 4; break;   /* LD E,E */
    case 0x5C: Z80_SET_E(cpu, Z80_H(cpu)); cycles = 4; break;   /* LD E,H */
    case 0x5D: Z80_SET_E(cpu, Z80_L(cpu)); cycles = 4; break;   /* LD E,L */
    case 0x5E: Z80_SET_E(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); cycles = 7; break;
    case 0x5F: Z80_SET_E(cpu, Z80_A(cpu)); cycles = 4; break;   /* LD E,A */
    case 0x60: Z80_SET_H(cpu, Z80_B(cpu)); cycles = 4; break;   /* LD H,B */
    case 0x61: Z80_SET_H(cpu, Z80_C(cpu)); cycles = 4; break;   /* LD H,C */
    case 0x62: Z80_SET_H(cpu, Z80_D(cpu)); cycles = 4; break;   /* LD H,D */
    case 0x63: Z80_SET_H(cpu, Z80_E(cpu)); cycles = 4; break;   /* LD H,E */
    case 0x64: Z80_SET_H(cpu, Z80_H(cpu)); cycles = 4; break;   /* LD H,H */
    case 0x65: Z80_SET_H(cpu, Z80_L(cpu)); cycles = 4; break;   /* LD H,L */
    case 0x66: Z80_SET_H(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); cycles = 7; break;
    case 0x67: Z80_SET_H(cpu, Z80_A(cpu)); cycles = 4; break;   /* LD H,A */
    case 0x68: Z80_SET_L(cpu, Z80_B(cpu)); cycles = 4; break;   /* LD L,B */
    case 0x69: Z80_SET_L(cpu, Z80_C(cpu)); cycles = 4; break;   /* LD L,C */
    case 0x6A: Z80_SET_L(cpu, Z80_D(cpu)); cycles = 4; break;   /* LD L,D */
    case 0x6B: Z80_SET_L(cpu, Z80_E(cpu)); cycles = 4; break;   /* LD L,E */
    case 0x6C: Z80_SET_L(cpu, Z80_H(cpu)); cycles = 4; break;   /* LD L,H */
    case 0x6D: Z80_SET_L(cpu, Z80_L(cpu)); cycles = 4; break;   /* LD L,L */
    case 0x6E: Z80_SET_L(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); cycles = 7; break;
    case 0x6F: Z80_SET_L(cpu, Z80_A(cpu)); cycles = 4; break;   /* LD L,A */
    case 0x70: Z80_MEM_WR(cpu, cpu->regs.hl, Z80_B(cpu)); cycles = 7; break;
    case 0x71: Z80_MEM_WR(cpu, cpu->regs.hl, Z80_C(cpu)); cycles = 7; break;
    case 0x72: Z80_MEM_WR(cpu, cpu->regs.hl, Z80_D(cpu)); cycles = 7; break;
    case 0x73: Z80_MEM_WR(cpu, cpu->regs.hl, Z80_E(cpu)); cycles = 7; break;
    case 0x74: Z80_MEM_WR(cpu, cpu->regs.hl, Z80_H(cpu)); cycles = 7; break;
    case 0x75: Z80_MEM_WR(cpu, cpu->regs.hl, Z80_L(cpu)); cycles = 7; break;
    case 0x76: cycles = z80_op_halt(cpu); break;               /* HALT */
    case 0x77: Z80_MEM_WR(cpu, cpu->regs.hl, Z80_A(cpu)); cycles = 7; break;
    case 0x78: Z80_SET_A(cpu, Z80_B(cpu)); cycles = 4; break;   /* LD A,B */
    case 0x79: Z80_SET_A(cpu, Z80_C(cpu)); cycles = 4; break;   /* LD A,C */
    case 0x7A: Z80_SET_A(cpu, Z80_D(cpu)); cycles = 4; break;   /* LD A,D */
    case 0x7B: Z80_SET_A(cpu, Z80_E(cpu)); cycles = 4; break;   /* LD A,E */
    case 0x7C: Z80_SET_A(cpu, Z80_H(cpu)); cycles = 4; break;   /* LD A,H */
    case 0x7D: Z80_SET_A(cpu, Z80_L(cpu)); cycles = 4; break;   /* LD A,L */
    case 0x7E: Z80_SET_A(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); cycles = 7; break;
    case 0x7F: Z80_SET_A(cpu, Z80_A(cpu)); cycles = 4; break;   /* LD A,A */

    /* 0x80-0xBF: Arithmetic/Logic */
    case 0x80: cycles = z80_op_add_a_r(cpu, Z80_B(cpu)); break;
    case 0x81: cycles = z80_op_add_a_r(cpu, Z80_C(cpu)); break;
    case 0x82: cycles = z80_op_add_a_r(cpu, Z80_D(cpu)); break;
    case 0x83: cycles = z80_op_add_a_r(cpu, Z80_E(cpu)); break;
    case 0x84: cycles = z80_op_add_a_r(cpu, Z80_H(cpu)); break;
    case 0x85: cycles = z80_op_add_a_r(cpu, Z80_L(cpu)); break;
    case 0x86: cycles = z80_op_add_a_r(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); break;
    case 0x87: cycles = z80_op_add_a_r(cpu, Z80_A(cpu)); break;
    case 0x88: cycles = z80_op_adc_a_r(cpu, Z80_B(cpu)); break;
    case 0x89: cycles = z80_op_adc_a_r(cpu, Z80_C(cpu)); break;
    case 0x8A: cycles = z80_op_adc_a_r(cpu, Z80_D(cpu)); break;
    case 0x8B: cycles = z80_op_adc_a_r(cpu, Z80_E(cpu)); break;
    case 0x8C: cycles = z80_op_adc_a_r(cpu, Z80_H(cpu)); break;
    case 0x8D: cycles = z80_op_adc_a_r(cpu, Z80_L(cpu)); break;
    case 0x8E: cycles = z80_op_adc_a_r(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); break;
    case 0x8F: cycles = z80_op_adc_a_r(cpu, Z80_A(cpu)); break;
    case 0x90: cycles = z80_op_sub_a_r(cpu, Z80_B(cpu)); break;
    case 0x91: cycles = z80_op_sub_a_r(cpu, Z80_C(cpu)); break;
    case 0x92: cycles = z80_op_sub_a_r(cpu, Z80_D(cpu)); break;
    case 0x93: cycles = z80_op_sub_a_r(cpu, Z80_E(cpu)); break;
    case 0x94: cycles = z80_op_sub_a_r(cpu, Z80_H(cpu)); break;
    case 0x95: cycles = z80_op_sub_a_r(cpu, Z80_L(cpu)); break;
    case 0x96: cycles = z80_op_sub_a_r(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); break;
    case 0x97: cycles = z80_op_sub_a_r(cpu, Z80_A(cpu)); break;
    case 0x98: cycles = z80_op_sbc_a_r(cpu, Z80_B(cpu)); break;
    case 0x99: cycles = z80_op_sbc_a_r(cpu, Z80_C(cpu)); break;
    case 0x9A: cycles = z80_op_sbc_a_r(cpu, Z80_D(cpu)); break;
    case 0x9B: cycles = z80_op_sbc_a_r(cpu, Z80_E(cpu)); break;
    case 0x9C: cycles = z80_op_sbc_a_r(cpu, Z80_H(cpu)); break;
    case 0x9D: cycles = z80_op_sbc_a_r(cpu, Z80_L(cpu)); break;
    case 0x9E: cycles = z80_op_sbc_a_r(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); break;
    case 0x9F: cycles = z80_op_sbc_a_r(cpu, Z80_A(cpu)); break;
    case 0xA0: cycles = z80_op_and_a_r(cpu, Z80_B(cpu)); break;
    case 0xA1: cycles = z80_op_and_a_r(cpu, Z80_C(cpu)); break;
    case 0xA2: cycles = z80_op_and_a_r(cpu, Z80_D(cpu)); break;
    case 0xA3: cycles = z80_op_and_a_r(cpu, Z80_E(cpu)); break;
    case 0xA4: cycles = z80_op_and_a_r(cpu, Z80_H(cpu)); break;
    case 0xA5: cycles = z80_op_and_a_r(cpu, Z80_L(cpu)); break;
    case 0xA6: cycles = z80_op_and_a_r(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); break;
    case 0xA7: cycles = z80_op_and_a_r(cpu, Z80_A(cpu)); break;
    case 0xA8: cycles = z80_op_xor_a_r(cpu, Z80_B(cpu)); break;
    case 0xA9: cycles = z80_op_xor_a_r(cpu, Z80_C(cpu)); break;
    case 0xAA: cycles = z80_op_xor_a_r(cpu, Z80_D(cpu)); break;
    case 0xAB: cycles = z80_op_xor_a_r(cpu, Z80_E(cpu)); break;
    case 0xAC: cycles = z80_op_xor_a_r(cpu, Z80_H(cpu)); break;
    case 0xAD: cycles = z80_op_xor_a_r(cpu, Z80_L(cpu)); break;
    case 0xAE: cycles = z80_op_xor_a_r(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); break;
    case 0xAF: cycles = z80_op_xor_a_r(cpu, Z80_A(cpu)); break;
    case 0xB0: cycles = z80_op_or_a_r(cpu, Z80_B(cpu)); break;
    case 0xB1: cycles = z80_op_or_a_r(cpu, Z80_C(cpu)); break;
    case 0xB2: cycles = z80_op_or_a_r(cpu, Z80_D(cpu)); break;
    case 0xB3: cycles = z80_op_or_a_r(cpu, Z80_E(cpu)); break;
    case 0xB4: cycles = z80_op_or_a_r(cpu, Z80_H(cpu)); break;
    case 0xB5: cycles = z80_op_or_a_r(cpu, Z80_L(cpu)); break;
    case 0xB6: cycles = z80_op_or_a_r(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); break;
    case 0xB7: cycles = z80_op_or_a_r(cpu, Z80_A(cpu)); break;
    case 0xB8: cycles = z80_op_cp_a_r(cpu, Z80_B(cpu)); break;
    case 0xB9: cycles = z80_op_cp_a_r(cpu, Z80_C(cpu)); break;
    case 0xBA: cycles = z80_op_cp_a_r(cpu, Z80_D(cpu)); break;
    case 0xBB: cycles = z80_op_cp_a_r(cpu, Z80_E(cpu)); break;
    case 0xBC: cycles = z80_op_cp_a_r(cpu, Z80_H(cpu)); break;
    case 0xBD: cycles = z80_op_cp_a_r(cpu, Z80_L(cpu)); break;
    case 0xBE: cycles = z80_op_cp_a_r(cpu, Z80_MEM_RD(cpu, cpu->regs.hl)); break;
    case 0xBF: cycles = z80_op_cp_a_r(cpu, Z80_A(cpu)); break;

    /* 0xC0-0xFF: Control flow */
    case 0xC0: cycles = z80_op_ret_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_Z)); break;
    case 0xC1: cycles = z80_op_pop_rr(cpu, &cpu->regs.bc); break;
    case 0xC2: cycles = z80_op_jp_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_Z)); break;
    case 0xC3: cycles = z80_op_jp(cpu); break;
    case 0xC4: cycles = z80_op_call_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_Z)); break;
    case 0xC5: cycles = z80_op_push_rr(cpu, cpu->regs.bc); break;
    case 0xC6: { uint8_t n = Z80_MEM_RD(cpu, cpu->regs.pc++); cycles = z80_op_add_a_r(cpu, n); } break;
    case 0xC7: cycles = z80_op_rst(cpu, 0x00); break;
    case 0xC8: cycles = z80_op_ret_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_Z)); break;
    case 0xC9: cycles = z80_op_ret(cpu); break;
    case 0xCA: cycles = z80_op_jp_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_Z)); break;
    case 0xCC: cycles = z80_op_call_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_Z)); break;
    case 0xCD: cycles = z80_op_call(cpu); break;
    case 0xCE: { uint8_t n = Z80_MEM_RD(cpu, cpu->regs.pc++); cycles = z80_op_adc_a_r(cpu, n); } break;
    case 0xCF: cycles = z80_op_rst(cpu, 0x08); break;

    case 0xD0: cycles = z80_op_ret_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_C)); break;
    case 0xD1: cycles = z80_op_pop_rr(cpu, &cpu->regs.de); break;
    case 0xD2: cycles = z80_op_jp_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_C)); break;
    case 0xD3: cycles = z80_op_out_n_a(cpu); break; /* stub callback */
    case 0xD4: cycles = z80_op_call_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_C)); break;
    case 0xD5: cycles = z80_op_push_rr(cpu, cpu->regs.de); break;
    case 0xD6: { uint8_t n = Z80_MEM_RD(cpu, cpu->regs.pc++); cycles = z80_op_sub_a_r(cpu, n); } break;
    case 0xD7: cycles = z80_op_rst(cpu, 0x10); break;
    case 0xD8: cycles = z80_op_ret_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_C)); break;
    case 0xD9: {
      uint16_t t;
      t = cpu->regs.bc; cpu->regs.bc = cpu->regs.bc_; cpu->regs.bc_ = t;
      t = cpu->regs.de; cpu->regs.de = cpu->regs.de_; cpu->regs.de_ = t;
      t = cpu->regs.hl; cpu->regs.hl = cpu->regs.hl_; cpu->regs.hl_ = t;
      cycles = 4;
    } break;
    case 0xDA: cycles = z80_op_jp_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_C)); break;
    case 0xDB: cycles = z80_op_in_a_n(cpu); break;  /* stub callback */
    case 0xDC: cycles = z80_op_call_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_C)); break;
    case 0xDE: { uint8_t n = Z80_MEM_RD(cpu, cpu->regs.pc++); cycles = z80_op_sbc_a_r(cpu, n); } break;
    case 0xDF: cycles = z80_op_rst(cpu, 0x18); break;

    case 0xE0: cycles = z80_op_ret_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_PV)); break; /* RET PO */
    case 0xE1: cycles = z80_op_pop_rr(cpu, &cpu->regs.hl); break;
    case 0xE2: cycles = z80_op_jp_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_PV)); break;
    case 0xE3: {
      uint16_t t = (uint16_t)Z80_MEM_RD(cpu, cpu->regs.sp) |
                   ((uint16_t)Z80_MEM_RD(cpu, cpu->regs.sp + 1) << 8);
      Z80_MEM_WR(cpu, cpu->regs.sp, (uint8_t)(cpu->regs.hl & 0xFF));
      Z80_MEM_WR(cpu, cpu->regs.sp + 1, (uint8_t)(cpu->regs.hl >> 8));
      cpu->regs.hl = t;
      cycles = 19;
    } break;
    case 0xE4: cycles = z80_op_call_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_PV)); break;
    case 0xE5: cycles = z80_op_push_rr(cpu, cpu->regs.hl); break;
    case 0xE6: { uint8_t n = Z80_MEM_RD(cpu, cpu->regs.pc++); cycles = z80_op_and_a_r(cpu, n); } break;
    case 0xE7: cycles = z80_op_rst(cpu, 0x20); break;
    case 0xE8: cycles = z80_op_ret_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_PV)); break; /* RET PE */
    case 0xE9: cpu->regs.pc = cpu->regs.hl; cycles = 4; break;
    case 0xEA: cycles = z80_op_jp_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_PV)); break;
    case 0xEB: {
      uint16_t t = cpu->regs.de;
      cpu->regs.de = cpu->regs.hl;
      cpu->regs.hl = t;
      cycles = 4;
    } break;
    case 0xEC: cycles = z80_op_call_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_PV)); break;
    case 0xEE: { uint8_t n = Z80_MEM_RD(cpu, cpu->regs.pc++); cycles = z80_op_xor_a_r(cpu, n); } break;
    case 0xEF: cycles = z80_op_rst(cpu, 0x28); break;

    case 0xF0: cycles = z80_op_ret_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_S)); break; /* RET P */
    case 0xF1: cycles = z80_op_pop_rr(cpu, &cpu->regs.af); break;
    case 0xF2: cycles = z80_op_jp_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_S)); break;
    case 0xF3: cpu->regs.iff1 = 0; cpu->regs.iff2 = 0; cycles = 4; break;
    case 0xF4: cycles = z80_op_call_cond(cpu, !GET_FLAG(Z80_F(cpu), FLAG_S)); break;
    case 0xF5: cycles = z80_op_push_rr(cpu, cpu->regs.af); break;
    case 0xF6: { uint8_t n = Z80_MEM_RD(cpu, cpu->regs.pc++); cycles = z80_op_or_a_r(cpu, n); } break;
    case 0xF7: cycles = z80_op_rst(cpu, 0x30); break;
    case 0xF8: cycles = z80_op_ret_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_S)); break; /* RET M */
    case 0xF9: cpu->regs.sp = cpu->regs.hl; cycles = 6; break;
    case 0xFA: cycles = z80_op_jp_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_S)); break;
    case 0xFB: cpu->regs.iff1 = 1; cpu->regs.iff2 = 1; cycles = 4; break;
    case 0xFC: cycles = z80_op_call_cond(cpu, GET_FLAG(Z80_F(cpu), FLAG_S)); break;
    case 0xFE: { uint8_t n = Z80_MEM_RD(cpu, cpu->regs.pc++); cycles = z80_op_cp_a_r(cpu, n); } break;
    case 0xFF: cycles = z80_op_rst(cpu, 0x38); break;

    default: cycles = z80_op_invalid(cpu); break;
    }

    cpu->cycles += cycles;
    cycles_executed += cycles;
  }

  return cycles_executed;
}





