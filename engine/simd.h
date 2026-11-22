/* AdvSIMD and FP/SIMD register instructions (the plan's M1 families). */
#ifndef XIAOJI_SIMD_H
#define XIAOJI_SIMD_H

#include <stdint.h>

#include "cpu_a64.h"

/* Executes one instruction from the "data processing: scalar floating-point
 * and Advanced SIMD" group (op0 = x111). Returns 0, or -1 if the encoding is not
 * implemented. */
int simd_exec(cpu_t *c, uint32_t insn);

/* Executes LD1/ST1 (multiple structures, one to four registers) with or without
 * post-index. Returns 0, or -1 if the encoding is not implemented. */
int simd_ldst_multi(cpu_t *c, uint32_t insn);

/* Executes LD1/ST1 (one lane) and LD1R (replicate), with or without post-index. */
int simd_ldst_single(cpu_t *c, uint32_t insn);

#endif
