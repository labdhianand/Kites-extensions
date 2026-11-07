# c[i] = a[i] + b[i] over 32 elements.
# a at offset 0, b at 128, c at 256 from the start of the data section (0x10000000).
.data
a:  .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
    .word 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32
b:  .word 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116
    .word 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132
c:  .zero 128

.text
    lui  x10, 0x10000        # &a
    addi x11, x0, 32         # n
    addi x12, x0, 0          # i
loop:
    slli x14, x12, 2
    add  x15, x10, x14
    lw   x18, 0(x15)         # a[i]
    lw   x19, 128(x15)       # b[i]
    add  x20, x18, x19
    sw   x20, 256(x15)       # c[i]
    addi x12, x12, 1
    blt  x12, x11, loop
