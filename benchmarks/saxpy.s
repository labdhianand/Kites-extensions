# y[i] = 3 * x[i] + y[i] over 32 elements (integer SAXPY).
# x at offset 0 and y at 128 from the start of the data section (0x10000000).
.data
x:  .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
    .word 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32
y:  .word 500, 501, 502, 503, 504, 505, 506, 507, 508, 509, 510, 511, 512, 513, 514, 515
    .word 516, 517, 518, 519, 520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531

.text
    lui  x10, 0x10000        # &x
    addi x9, x0, 3           # the scale factor
    addi x11, x0, 32         # n
    addi x12, x0, 0          # i
loop:
    slli x14, x12, 2
    add  x15, x10, x14
    lw   x18, 0(x15)         # x[i]
    mul  x18, x18, x9
    lw   x19, 128(x15)       # y[i]
    add  x19, x19, x18
    sw   x19, 128(x15)       # y[i]
    addi x12, x12, 1
    blt  x12, x11, loop
