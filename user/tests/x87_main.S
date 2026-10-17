/* Test fixture (never shipped): main() that uses x87, which ABI v0 forbids. */
.section .text
.global main
.type main, @function
main:
    fld1
    xor %eax, %eax
    ret
.section .note.GNU-stack, "", @progbits
