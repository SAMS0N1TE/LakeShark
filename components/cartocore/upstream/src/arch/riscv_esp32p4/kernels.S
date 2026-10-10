/* Hand-written CartoCore kernels. Internal ABI: aligned, whole blocks only.
 * Task ownership/PIE enablement and tails are checked by arch.c.
 * Only caller-saved integer registers and q0..q2 are used. */
#ifdef CC_HAVE_ESP32P4_PIE
    .text
    .p2align 2
    .global cc_pie_task_enter
    .type cc_pie_task_enter,@function
cc_pie_task_enter:
    /* With PIE off this enters IDF's lazy coprocessor ownership handler. */
    esp.xorq q0,q0,q0
    ret
    .size cc_pie_task_enter,.-cc_pie_task_enter

    .macro bitrow name,op
    .section .text.\name,"ax",@progbits
    .p2align 2
    .global \name
    .type \name,@function
\name:
    beqz a2,2f
1:
    mv a3,a0
    esp.vld.128.ip q0,a3,16
    esp.vld.128.ip q1,a1,16
    \op q0,q0,q1
    esp.vst.128.ip q0,a0,16
    addi a2,a2,-1
    bnez a2,1b
2:
    ret
    .size \name,.-\name
    .endm
    bitrow cc_pie_or,esp.orq
    bitrow cc_pie_and,esp.andq
    bitrow cc_pie_xor,esp.xorq

    .section .text.cc_pie_clear,"ax",@progbits
    .p2align 2
    .global cc_pie_clear
    .type cc_pie_clear,@function
cc_pie_clear:
    beqz a1,2f
    esp.xorq q0,q0,q0
1:
    esp.vst.128.ip q0,a0,16
    addi a1,a1,-1
    bnez a1,1b
2:
    ret
    .size cc_pie_clear,.-cc_pie_clear

    .section .text.cc_pie_copy,"ax",@progbits
    .p2align 2
    .global cc_pie_copy
    .type cc_pie_copy,@function
cc_pie_copy:
    beqz a2,2f
1:
    esp.vld.128.ip q0,a1,16
    esp.vst.128.ip q0,a0,16
    addi a2,a2,-1
    bnez a2,1b
2:
    ret
    .size cc_pie_copy,.-cc_pie_copy

    .section .text.cc_pie_set,"ax",@progbits
    .p2align 2
    .global cc_pie_set
    .type cc_pie_set,@function
cc_pie_set:
    beqz a2,2f
    esp.vld.128.ip q0,a1,16
1:
    esp.vst.128.ip q0,a0,16
    addi a2,a2,-1
    bnez a2,1b
2:
    ret
    .size cc_pie_set,.-cc_pie_set

    /* Four packed 12-byte cc_cells per group. XOR/OR reduction avoids
     * assuming comparison-mask representation. Stops at first changed group. */
    .section .text.cc_pie_equal48,"ax",@progbits
    .p2align 2
    .global cc_pie_equal48
    .type cc_pie_equal48,@function
cc_pie_equal48:
    addi sp,sp,-16
    li t4,0
    beqz a2,2f
1:
    esp.vld.128.ip q0,a0,16
    esp.vld.128.ip q1,a1,16
    esp.xorq q2,q0,q1
    esp.vld.128.ip q0,a0,16
    esp.vld.128.ip q1,a1,16
    esp.xorq q0,q0,q1
    esp.orq q2,q2,q0
    esp.vld.128.ip q0,a0,16
    esp.vld.128.ip q1,a1,16
    esp.xorq q0,q0,q1
    esp.orq q2,q2,q0
    mv a3,sp
    esp.vst.128.ip q2,a3,16
    lw t0,0(sp)
    lw t1,4(sp)
    lw t2,8(sp)
    lw t3,12(sp)
    or t0,t0,t1
    or t2,t2,t3
    or t0,t0,t2
    bnez t0,2f
    addi t4,t4,1
    addi a2,a2,-1
    bnez a2,1b
2:
    mv a0,t4
    addi sp,sp,16
    ret
    .size cc_pie_equal48,.-cc_pie_equal48
#endif
