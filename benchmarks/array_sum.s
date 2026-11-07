# Sum of a 32-element array.
# The data section starts at 0x10000000, so `lui x10, 0x10000` points at its start; arrays follow
# each other in the order they are declared. (`la` is avoided on purpose: it expands to two
# dependent instructions that cannot be padded apart on a pipeline without hazard detection.)
.data
a:      .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
        .word 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32
result: .word 0

.text
    lui  x10, 0x10000        # &a
    addi x11, x0, 32         # n
    addi x12, x0, 0          # i
    addi x13, x0, 0          # sum
loop:
    slli x14, x12, 2
    add  x15, x10, x14
    lw   x16, 0(x15)         # a[i]
    add  x13, x13, x16
    addi x12, x12, 1
    blt  x12, x11, loop
    sw   x13, 128(x10)       # result
