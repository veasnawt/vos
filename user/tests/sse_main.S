/* Test fixture (never shipped): main() that uses SSE, which ABI v0 forbids. */
.section .text
.global main
.type main, @function
main:
    xorps %xmm0, %xmm0
    xor %eax, %eax
    ret
.section .note.GNU-stack, "", @progbits
