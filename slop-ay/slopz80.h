/* slopz80.h
 *
 * Z80 CPU emulator declarations.
 *
 * Copyright (c) David Thomas, 2026. <dave@davespace.co.uk>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SLOPZ80_H
#define SLOPZ80_H

#include <stdint.h>

typedef struct {
  uint16_t af, bc, de, hl;
  uint16_t af_, bc_, de_, hl_;
  uint16_t ix, iy;
  uint16_t sp, pc;
  uint8_t i, r;
  uint8_t iff1, iff2, im;
  uint8_t halted;
} slopz80_regs_t;

typedef uint8_t (*slopz80_port_read_fn)(void *ctx, uint16_t port);
typedef void (*slopz80_port_write_fn)(void *ctx, uint16_t port, uint8_t val);

typedef struct {
  slopz80_regs_t regs;
  uint8_t *mem;
  uint32_t cycles;
  slopz80_port_read_fn port_read;
  slopz80_port_write_fn port_write;
  void *port_ctx;
} slopz80_t;

typedef struct {
  uint32_t dd_counts[256];
  uint32_t fd_counts[256];
  uint32_t ed_counts[256];
} slopz80_missing_opcode_stats_t;

/* Flag bits */
#define Z80_FLAG_C  0x01  /* Carry */
#define Z80_FLAG_N  0x02  /* Add/Subtract */
#define Z80_FLAG_PV 0x04  /* Parity/Overflow */
#define Z80_FLAG_H  0x10  /* Half-carry */
#define Z80_FLAG_Z  0x40  /* Zero */
#define Z80_FLAG_S  0x80  /* Sign */

/* Helpers for register access */
#define Z80_A(cpu) (((cpu)->regs.af >> 8) & 0xFF)
#define Z80_F(cpu) ((cpu)->regs.af & 0xFF)
#define Z80_B(cpu) (((cpu)->regs.bc >> 8) & 0xFF)
#define Z80_C(cpu) ((cpu)->regs.bc & 0xFF)
#define Z80_D(cpu) (((cpu)->regs.de >> 8) & 0xFF)
#define Z80_E(cpu) ((cpu)->regs.de & 0xFF)
#define Z80_H(cpu) (((cpu)->regs.hl >> 8) & 0xFF)
#define Z80_L(cpu) ((cpu)->regs.hl & 0xFF)

#define Z80_SET_A(cpu, v) ((cpu)->regs.af = ((cpu)->regs.af & 0x00FF) | ((v) << 8))
#define Z80_SET_F(cpu, v) ((cpu)->regs.af = ((cpu)->regs.af & 0xFF00) | (v))
#define Z80_SET_B(cpu, v) ((cpu)->regs.bc = ((cpu)->regs.bc & 0x00FF) | ((v) << 8))
#define Z80_SET_C(cpu, v) ((cpu)->regs.bc = ((cpu)->regs.bc & 0xFF00) | (v))
#define Z80_SET_D(cpu, v) ((cpu)->regs.de = ((cpu)->regs.de & 0x00FF) | ((v) << 8))
#define Z80_SET_E(cpu, v) ((cpu)->regs.de = ((cpu)->regs.de & 0xFF00) | (v))
#define Z80_SET_H(cpu, v) ((cpu)->regs.hl = ((cpu)->regs.hl & 0x00FF) | ((v) << 8))
#define Z80_SET_L(cpu, v) ((cpu)->regs.hl = ((cpu)->regs.hl & 0xFF00) | (v))

/* Memory access */
#define Z80_MEM_RD(cpu, addr) ((cpu)->mem[(addr) & 0xFFFF])
#define Z80_MEM_WR(cpu, addr, val) ((cpu)->mem[(addr) & 0xFFFF] = (val))

/* Public functions */
slopz80_t *slopz80_create(uint8_t *memory);
void slopz80_destroy(slopz80_t *cpu);
void slopz80_reset(slopz80_t *cpu);
int slopz80_execute(slopz80_t *cpu, int max_cycles);
void slopz80_missing_opcode_reset(void);
void slopz80_missing_opcode_snapshot(slopz80_missing_opcode_stats_t *out_stats);
void slopz80_set_port_callbacks(slopz80_t *cpu,
                                slopz80_port_read_fn read_fn,
                                slopz80_port_write_fn write_fn,
                                void *ctx);

#endif /* SLOPZ80_H */


