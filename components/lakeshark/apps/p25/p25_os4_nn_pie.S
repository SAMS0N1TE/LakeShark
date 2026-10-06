/* The symbol network's two big layers on the P4's vector unit (PIE), for
   p25_os4_nn.c. The instruction forms follow esp-dsp's ESP32-P4 kernels.
   q3 is never used: ESP-IDF 5.4.3 saves q0-q2 and q4-q7 when another task
   takes the unit over, and not q3. Nor is a hardware loop (esp.lp.setup):
   on this chip revision the loop state is not marked dirty (IDF's
   SOC_CPU_HAS_HWLOOP_STATE_BUG), and a kernel preempted inside one came back
   with another count, about one symbol in two thousand wrong on the chip
   bench, different every run. Run these only on a task whose stack is not
   in the LP SRAM: the unit's registers are saved at the stack's bottom and
   it cannot store there. */

#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32P4

    .text
    .align  4

/* void p25_os4_nn_l1_pie(const int16_t *const rows[16], const int16_t *bias,
                          int16_t *out, int passes)
   out = bias + the sixteen rows, in saturating int16 lanes, 32 lanes a pass.
   Rows, bias and out are 16-byte aligned. a0 rows, a1 bias, a2 out, a3 passes. */
    .global p25_os4_nn_l1_pie
    .type   p25_os4_nn_l1_pie, @function
p25_os4_nn_l1_pie:
    li      t2, 0
.l1_pass:
    add     t5, a1, t2
    esp.vld.128.ip  q0, t5, 16
    esp.vld.128.ip  q1, t5, 16
    esp.vld.128.ip  q2, t5, 16
    esp.vld.128.ip  q4, t5, 16
    mv      t3, a0
    li      t4, 16
.l1_row:
    lw      t5, 0(t3)
    add     t5, t5, t2
    esp.vld.128.ip  q5, t5, 16
    esp.vld.128.ip  q6, t5, 16
    esp.vld.128.ip  q7, t5, 16
    esp.vadd.s16    q0, q0, q5
    esp.vadd.s16    q1, q1, q6
    esp.vadd.s16    q2, q2, q7
    esp.vld.128.ip  q5, t5, 16
    esp.vadd.s16    q4, q4, q5
    addi    t3, t3, 4
    addi    t4, t4, -1
    bnez    t4, .l1_row
    add     t5, a2, t2
    esp.vst.128.ip  q0, t5, 16
    esp.vst.128.ip  q1, t5, 16
    esp.vst.128.ip  q2, t5, 16
    esp.vst.128.ip  q4, t5, 16
    addi    t2, t2, 64
    addi    a3, a3, -1
    bnez    a3, .l1_pass
    ret
    .size   p25_os4_nn_l1_pie, . - p25_os4_nn_l1_pie

/* void p25_os4_nn_l2_pie(const int16_t *a, const int16_t *wt, int16_t *out,
                          int n, int k, int shift)
   out[j] = (sum over s < n of a[s] * wt[s][j]) >> (15 - shift), saturated to
   int16, for j < k: a matrix-vector product with the weights transposed (n
   rows of k), eight outputs at a time, as esp-dsp's dspm_mult_s16 does a row.
   n is a multiple of 4, k of 8; wt and out are 16-byte aligned.
   a0 a, a1 wt, a2 out, a3 n, a4 k, a5 shift. */
    .global p25_os4_nn_l2_pie
    .type   p25_os4_nn_l2_pie, @function
p25_os4_nn_l2_pie:
    addi    sp, sp, -16
    sw      s8, 4(sp)
    sw      s9, 8(sp)
    sw      s10, 12(sp)
    slli    s9, a4, 1
    li      t6, 2
    li      s10, 15
    sub     s10, s10, a5       /* srcmb takes the right shift itself, 15 - shift */
    li      t1, 0
    mv      t4, a1
.l2_block:
    mv      t5, a0
    mv      s8, t4
    esp.zero.qacc
    esp.vldbc.16.xp     q0, t5, t6
    esp.vld.128.xp      q1, s8, s9
    srli    t3, a3, 2
.l2_loop:
    esp.vmulas.s16.qacc.ldbc.incp   q0, t5, q0, q1
    esp.vld.128.xp      q1, s8, s9
    esp.vmulas.s16.qacc.ldbc.incp   q0, t5, q0, q1
    esp.vld.128.xp      q1, s8, s9
    esp.vmulas.s16.qacc.ldbc.incp   q0, t5, q0, q1
    esp.vld.128.xp      q1, s8, s9
    esp.vmulas.s16.qacc.ldbc.incp   q0, t5, q0, q1
    esp.vld.128.xp      q1, s8, s9
    addi    t3, t3, -1
    bnez    t3, .l2_loop
    esp.srcmb.s16.qacc  q2, s10, 0
    esp.vst.128.ip      q2, a2, 16
    addi    t4, t4, 16
    addi    t1, t1, 8
    blt     t1, a4, .l2_block
    lw      s10, 12(sp)
    lw      s9, 8(sp)
    lw      s8, 4(sp)
    addi    sp, sp, 16
    ret
    .size   p25_os4_nn_l2_pie, . - p25_os4_nn_l2_pie

/* int32_t p25_os4_nn_dot_pie(const int16_t *a, const int16_t *b, int n8, int shift)
   (the sum of a[i] * b[i] over 8 * n8 terms) >> shift, saturated to int32,
   in the 40-bit accumulator as esp-dsp's dsps_dotprod_s16 does it. a and b
   are 16-byte aligned. a0 a, a1 b, a2 n8, a3 shift. */
    .global p25_os4_nn_dot_pie
    .type   p25_os4_nn_dot_pie, @function
p25_os4_nn_dot_pie:
    esp.zero.xacc
    mv      t3, a0
    mv      t4, a1
    esp.vld.128.ip  q0, t3, 16
.dot_loop:
    esp.vld.128.ip  q1, t4, 16
    esp.vmulas.s16.xacc.ld.ip   q0, t3, 16, q0, q1
    addi    a2, a2, -1
    bnez    a2, .dot_loop
    esp.srs.s.xacc  a0, a3
    ret
    .size   p25_os4_nn_dot_pie, . - p25_os4_nn_dot_pie

#endif
