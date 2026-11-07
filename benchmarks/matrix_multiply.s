# C = A * B for 4x4 integer matrices, row-major.
# A at offset 0, B at 64 and C at 128 from the start of the data section (0x10000000).
# Only the innermost loop is a single block with a fixed trip count, so only it can be unrolled.
.data
A:  .word 1, 2, 3, 4
    .word 5, 6, 7, 8
    .word 9, 10, 11, 12
    .word 13, 14, 15, 16
B:  .word 2, 0, 1, 3
    .word 1, 4, 0, 2
    .word 3, 1, 5, 0
    .word 0, 2, 1, 4
C:  .zero 64

.text
    lui  x10, 0x10000        # &A
    addi x20, x0, 4          # N
    addi x5, x0, 0           # i
iloop:
    addi x6, x0, 0           # j
jloop:
    addi x7, x0, 0           # k
    addi x8, x0, 0           # C[i][j] accumulator
kloop:
    slli x9, x5, 4           # i * 16 bytes per row
    slli x11, x7, 2          # k * 4
    add  x12, x9, x11
    add  x12, x12, x10
    lw   x13, 0(x12)         # A[i][k]
    slli x14, x7, 4          # k * 16
    slli x15, x6, 2          # j * 4
    add  x16, x14, x15
    add  x16, x16, x10
    lw   x17, 64(x16)        # B[k][j]
    mul  x18, x13, x17
    add  x8, x8, x18
    addi x7, x7, 1
    blt  x7, x20, kloop
    slli x9, x5, 4
    slli x15, x6, 2
    add  x16, x9, x15
    add  x16, x16, x10
    sw   x8, 128(x16)        # C[i][j]
    addi x6, x6, 1
    blt  x6, x20, jloop
    addi x5, x5, 1
    blt  x5, x20, iloop
