/* Original LakeShark GBA cartridge. The built-in open BIOS boots it. */
.arm
.section .header,"ax"
.global _start
_start:
    b boot
    .space 156
    .ascii "SIGNAL CATCH"
    .ascii "LSC1"
    .ascii "00"
    .byte 0x96, 0, 0
    .space 8
    .byte 0, 0, 0
boot:
    ldr sp, =0x03007f00
    b game_main
